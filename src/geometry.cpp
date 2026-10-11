#include "geometry.hpp"
#include <tiny_obj_loader.h>
#include <algorithm>

float scaleToFit(const TriMesh &mesh, const Grid &g);

void applyAoA(TriMesh& mesh, float aoaDeg);

void fitToGrid(TriMesh& mesh, const Grid& grid, float aoaDeg, bool zUp);

void fixUpAxis(TriMesh &mesh, bool zUp);

bool loadMesh(const std::string& path, TriMesh& out){
    out = {};
    tinyobj::ObjReaderConfig config;
    config.mtl_search_path = "./";
    config.triangulate = true;
    tinyobj::ObjReader reader;

    if (!reader.ParseFromFile(path, config)){
        if (!reader.Error().empty())
            fprintf(stderr, "%s\n", reader.Error().c_str());

        return false;
    }

    auto &attrib = reader.GetAttrib();
    auto &shapes = reader.GetShapes();


    /* convert the flat float array into an array of vec3s */
    out.vertices.reserve(attrib.vertices.size() / 3);
    for (size_t i = 0; i + 2 < attrib.vertices.size(); i += 3)
        out.vertices.emplace_back(
            attrib.vertices[i],
            attrib.vertices[i + 1],
            attrib.vertices[i + 2]
        );

    /* indices are uint16_t, so anything bigger can't be addressed */
    if (out.vertices.size() > UINT16_MAX)
        return false;

    /* merge all the triangles into one list */
    for (const auto &shape : shapes){
        const auto &ids = shape.mesh.indices;

        for (size_t i = 0; i + 2 < ids.size(); i += 3){
            uint16_t a = (uint16_t)ids[i].vertex_index,
                     b = (uint16_t)ids[i + 1].vertex_index,
                     c = (uint16_t)ids[i + 2].vertex_index
            ;

            /* check if the triangle has area (degenerate) */
            glm::vec3 n = glm::cross(
                out.vertices[b] - out.vertices[a],
                out.vertices[c] - out.vertices[a]
            );

            if (glm::dot(n, n) < 1e-20f)
                continue;

            out.indices.insert(out.indices.end(), {a, b, c});
        }
    }

    if (out.vertices.empty())
        return false;

    /**
     * compute our own normals since OBJ files don't normally include them
     */

    out.normals.assign(out.vertices.size(), glm::vec3{0.0f});
    for (size_t t = 0; t < out.indices.size(); t += 3){
        uint16_t a = out.indices[t],
                 b = out.indices[t + 1],
                 c = out.indices[t + 2]
        ;

        glm::vec3 n = glm::cross(
            out.vertices[b] - out.vertices[a],
            out.vertices[c] - out.vertices[a]
        );

        out.normals[a] += n;
        out.normals[b] += n;
        out.normals[c] += n;
    }

    for (auto &n : out.normals)
        n = glm::length(n) > 0.0f ?
            glm::normalize(n) :
            glm::vec3(0, 1, 0)
        ;

    return true;
}

/* axis-aligned min/max corners of the mesh's vertices */
void boundingBox(const TriMesh &mesh, glm::vec3 &outLow, glm::vec3 &outHi){
    outLow = glm::vec3{1e30f};
    outHi = glm::vec3{-1e30f};

    for (const auto &vert : mesh.vertices){
        outLow = glm::min(outLow, vert);
        outHi = glm::max(outHi, vert);
    }
}

/* convert a Z-up model to Y-up: (x, y, z) -> (x, z, -y). no-op if !zUp */
void fixUpAxis(TriMesh &mesh, bool zUp){
    if (!zUp)
        return;

    for (auto &v : mesh.vertices)
        v = {v.x, v.z, -v.y};

    for (auto &n : mesh.normals)
        n = {n.x, n.z, -n.y};
}

void fitToGrid(TriMesh& mesh, const Grid& grid, float aoaDeg, bool zUp){
    fixUpAxis(mesh, zUp);

    glm::vec3 low, hi;
    boundingBox(mesh, low, hi);

    glm::vec3 center = (low + hi) / 2.0f;

    for (auto &vert : mesh.vertices)
         vert -= center;
    
    applyAoA(mesh, aoaDeg);

    float s = scaleToFit(mesh, grid);

    glm::vec3 target {
        grid.nx / 2.0f,
        grid.ny / 2.0f,
        grid.nz / 2.0f
    };

    for (auto &v : mesh.vertices)
        v = v * s + target;
}

/* pitch about Z through the origin. positive = nose up (nose faces -X). mesh must be centered */
void applyAoA(TriMesh& mesh, float aoaDeg){
    float a = glm::radians(-aoaDeg);
    float ca = std::cos(a),
          sa = std::sin(a)
    ;

    auto rotate = [&](glm::vec3 &v){
        v = {
            ca * v.x - sa * v.y,
            sa * v.x + ca * v.y,
            v.z
        };
    };

    std::for_each(mesh.vertices.begin(), mesh.vertices.end(), rotate);
    std::for_each(mesh.normals.begin(), mesh.normals.end(), rotate);
}

/* uniform scale that fits the mesh in the tunnel and keeps blockage <= 12%. doesn't modify the mesh */
float scaleToFit(const TriMesh &mesh, const Grid &g){
    glm::vec3 low, high;
    boundingBox(mesh, low, high);

    glm::vec3 extent = glm::max(high - low, glm::vec3{1e-6f}); /* avoid / 0 on flat meshes ... */

    /**
     * take the min of three ratios (making the model fit on x, y, or z axis)
     * the tightest one is the ratio we use to scale the entire model
     */
    float s = std::min({
        0.50f * g.nx / extent.x,
        0.50f * g.ny / extent.y,
        0.50f * g.nz / extent.z
    });

    /* fraction that the model's cross-section covers of the tunnel */
    float blockage = (extent.y * s) * (extent.z * s) / (float)(g.ny * g.nz);
    if (blockage > 0.12f)
        s *= std::sqrt(0.12f / blockage);

    return s;
}

void voxelize(const TriMesh& mesh, Grid& grid){
    grid.solid.assign(grid.nx * grid.ny * grid.nz, 0);

    std::vector<std::vector<float>> hits ((size_t) grid.ny * grid.nz);

    /**
     * iterate through the triangles,
     * for every triangle, creating a bounding box in the YZ plane
     * see which columns that it intersects
     */

    for (size_t t = 0; t < mesh.indices.size(); t += 3){
        glm::vec3 a = mesh.vertices[mesh.indices[t]],
                  b = mesh.vertices[mesh.indices[t + 1]],
                  c = mesh.vertices[mesh.indices[t + 2]]
        ;

        float ymin = std::min({a.y, b.y, c.y}),
              yhigh = std::max({a.y, b.y, c.y}),
              zmin = std::min({a.z, b.z, c.z}),
              zhigh = std::max({a.z, b.z, c.z})
        ;

        /* ray sits at middle of voxel, but the bounding box is at edges */

        int j0 = std::max(0,      (int)std::ceil (ymin - 0.5f));
        int j1 = std::min(grid.ny - 1, (int)std::floor(yhigh - 0.5f));
        int k0 = std::max(0,      (int)std::ceil (zmin - 0.5f));
        int k1 = std::min(grid.nz - 1, (int)std::floor(zhigh - 0.5f));

        /* barycentric coordinates to test if points are inside triangle */

        float d = (b.y - a.y) * (c.z - a.z) - (c.y - a.y) * (b.z - a.z);

        if (std::abs(d) < 1e-12) continue; /* edge-on to the ray, can't be hit */

        for (int k = k0; k <= k1; ++k){
            for (int j = j0; j <= j1; ++j){
                /* point within the bounding box that we are testing */
                float py = j + 0.5f + 1e-4f;
                float pz = k + 0.5f + 1.7e-4f;

                float wA = ((b.y - py) * (c.z - pz) - (c.y - py) * (b.z - pz)) / d,
                      wB = ((c.y - py) * (a.z - pz) - (a.y - py) * (c.z - pz)) / d
                ;
                float wC = 1.0f - wA - wB;

                if (wA < 0.f || wB < 0.f || wC < 0.f)
                    continue; /* ray missed */

                float x = wA * a.x + wB * b.x + wC * c.x; /* where it hit */
                hits[j + (size_t)grid.ny * k].push_back(x);
            }
        }
    }

    /* now that every triangle is in, sort the hits and fill between in/out pairs */

    for (int k = 0; k < grid.nz; ++k){
        for (int j = 0; j < grid.ny; ++j){
            std::vector<float> &h = hits[j + (size_t)grid.ny * k];

            /* an odd count means a leaky mesh, the lone last hit just gets ignored */
            std::sort(h.begin(), h.end());

            for (size_t p = 0; p + 1 < h.size(); p += 2){
                /* range for that point in the YZ plane */
                int i0 = std::max(0, (int)std::ceil(h[p] - 0.5f));
                int i1 = std::min(grid.nx - 1, (int)std::ceil(h[p + 1] - 0.5f) - 1);

                for (int i = i0; i <= i1; ++i)
                    grid.solid[i + (size_t)grid.nx * (j + (size_t)grid.ny * k)] = 1;
            }
        }
    }
}

TriMesh makeSphere();
TriMesh makeCube();
TriMesh makeNacaWing();
TriMesh makeCar();

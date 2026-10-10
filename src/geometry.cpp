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

    /* merge all the triangles into one list */
    for (const auto &shape : shapes){
        const auto &ids = shape.mesh.indices;

        for (size_t i = 0; i + 2 < ids.size(); i += 3){
            uint32_t a = ids[i].vertex_index,
                     b = ids[i + 1].vertex_index,
                     c = ids[i + 2].vertex_index
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
        uint32_t a = out.indices[t],
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
    glm::vec3 low, hi;
    boundingBox(mesh, low, hi);

    for (const auto &vert : mesh.vertices){
        low = glm::min(low, vert);
        hi = glm::max(hi, vert);
    }

    glm::vec3 center = (low + hi) / 2.0f;

    for (auto &vert : mesh.vertices)
         vert -= center;
    
    float s = scaleToFit(mesh, grid);

    glm::vec3 target {
        grid.nx / 4.0f,
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

    glm::vec3 extent = glm::max(high - low, glm::vec3{-1e30f}); /* avoid / 0 on flat meshes ... */

    /**
     * take the min of three ratios (making the model fit on x, y, or z axis)
     * the tightest one is the ratio we use to scale the entire model
     */
    float s = std::min({
        0.30f * g.nx / extent.x,
        0.35f * g.ny / extent.y,
        0.30f * g.nz / extent.z
    });

    /* fraction that the model's cross-section covers of the tunnel */
    float blockage = (extent.y * s) * (extent.z * s) / (float)(g.ny * g.nz);
    if (blockage > 0.12f)
        s *= std::sqrt(0.12f / blockage);

    return s;
}

void voxelize(const TriMesh& mesh, Grid& grid);
float frontalArea(const Grid& grid);
TriMesh makeSphere();
TriMesh makeCube();
TriMesh makeNacaWing();
TriMesh makeCar();

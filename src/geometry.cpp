#include "geometry.hpp"
#include <tiny_obj_loader.h>
#include <print>

bool loadMesh(const std::string& path, TriMesh& out){
    tinyobj::ObjReaderConfig config;
    config.mtl_search_path = "./";
    config.triangulate = true;
    tinyobj::ObjReader reader;

    if (!reader.ParseFromFile(path)){
        if (!reader.Error().empty())
            std::println("{}", reader.Error());

        return false;
    }

    if (!reader.Warning().empty())
        std::println("{}", reader.Warning());

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

            out.vertices.emplace_back(a, b, c);
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

void    fitToGrid(TriMesh& mesh, const Grid& grid);
void    applyAoA(TriMesh& mesh, float aoaDeg);
void    voxelize(const TriMesh& mesh, Grid& grid);
float   frontalArea(const Grid& grid);
TriMesh makeSphere();
TriMesh makeCube();
TriMesh makeNacaWing();
TriMesh makeCar();

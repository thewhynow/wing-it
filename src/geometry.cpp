#include "geometry.hpp"
#include <tiny_obj_loader.h>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

float scaleToFit(const TriMesh &mesh, const Grid &g);

void applyAoA(TriMesh& mesh, float aoaDeg);

void fitToGrid(TriMesh& mesh, const Grid& grid, float aoaDeg, bool zUp);

void fixUpAxis(TriMesh &mesh, bool zUp);

/* ---------------------------------------------------------------------------
 * Mesh loading: OBJ (tinyobjloader), STL (ASCII + binary) and PLY (ASCII + binary LE/BE).
 * Every format is parsed into a RawMesh (vec3 + uint32 triangles), then finalizeMesh() validates it,
 * drops unused vertices, decimates by vertex clustering if it has more than 65535 vertices
 * (TriMesh indices are uint16), drops degenerate triangles and computes normals.
 * ------------------------------------------------------------------------- */
namespace {

struct RawMesh {
    std::vector<glm::vec3> v;
    std::vector<uint32_t> t; // 3 per triangle
};

std::string lowerExt(const std::string &path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

bool readWholeFile(const std::string &path, std::string &data, std::string &err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "can't open the file"; return false; }
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0) { err = "can't read the file"; return false; }
    f.seekg(0, std::ios::beg);
    data.resize((size_t)n);
    if (n > 0 && !f.read(data.data(), n)) { err = "can't read the file"; return false; }
    return true;
}

/* ---- OBJ ---- */
bool loadObj(const std::string &path, RawMesh &m, std::string &err) {
    tinyobj::ObjReaderConfig config;
    config.mtl_search_path = "./";
    config.triangulate = true;
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, config)) {
        err = reader.Error().empty() ? "not a valid OBJ file" : "OBJ: " + reader.Error();
        while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
        return false;
    }
    const auto &attrib = reader.GetAttrib();
    m.v.reserve(attrib.vertices.size() / 3);
    for (size_t i = 0; i + 2 < attrib.vertices.size(); i += 3)
        m.v.emplace_back(attrib.vertices[i], attrib.vertices[i + 1], attrib.vertices[i + 2]);
    for (const auto &shape : reader.GetShapes()) {
        const auto &ids = shape.mesh.indices;
        for (size_t i = 0; i + 2 < ids.size(); i += 3)
            for (int k = 0; k < 3; ++k) {
                int vi = ids[i + k].vertex_index;
                m.t.push_back(vi < 0 ? UINT32_MAX : (uint32_t)vi); // bad index -> rejected by finalizeMesh
            }
    }
    return true;
}

/* ---- STL ---- */
struct Vec3Hash {
    size_t operator()(const glm::vec3 &p) const {
        uint32_t b[3];
        std::memcpy(b, &p, sizeof b);
        uint64_t h = 1469598103934665603ull;
        for (uint32_t x : b) { h ^= x; h *= 1099511628211ull; }
        return (size_t)h;
    }
};

/* STL stores every triangle with its own 3 corners - weld identical positions back together */
void weldInto(const std::vector<glm::vec3> &corners, RawMesh &m) {
    std::unordered_map<glm::vec3, uint32_t, Vec3Hash> ids;
    ids.reserve(corners.size() / 2);
    m.t.reserve(corners.size());
    for (glm::vec3 p : corners) {
        p += glm::vec3(0.0f); // -0 -> +0 so they hash the same
        auto [it, fresh] = ids.try_emplace(p, (uint32_t)m.v.size());
        if (fresh) m.v.push_back(p);
        m.t.push_back(it->second);
    }
}

bool loadStl(const std::string &data, RawMesh &m, std::string &err) {
    std::vector<glm::vec3> corners;
    uint32_t n = 0;
    if (data.size() >= 84) std::memcpy(&n, data.data() + 80, 4);
    /* many binary STLs start with "solid" too, so decide by size: binary is exactly 84 + 50 * n bytes */
    if (data.size() >= 84 && (uint64_t)data.size() == 84ull + 50ull * n) {
        corners.reserve((size_t)n * 3);
        for (uint32_t i = 0; i < n; ++i) {
            const char *rec = data.data() + 84 + 50ull * i + 12; // skip the facet normal
            for (int k = 0; k < 3; ++k) {
                float xyz[3];
                std::memcpy(xyz, rec + 12 * k, 12);
                corners.emplace_back(xyz[0], xyz[1], xyz[2]);
            }
        }
    } else {
        /* ASCII: every "vertex x y z" line, three per facet */
        size_t pos = 0;
        while ((pos = data.find("vertex", pos)) != std::string::npos) {
            pos += 6;
            const char *p = data.c_str() + pos;
            char *end;
            float xyz[3];
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
                xyz[k] = std::strtof(p, &end);
                ok = end != p;
                p = end;
            }
            if (!ok) continue; // e.g. "endvertex" style junk
            corners.emplace_back(xyz[0], xyz[1], xyz[2]);
        }
        if (corners.empty()) {
            err = data.compare(0, 5, "solid") == 0 ? "STL file has no facets" : "not a valid STL file";
            return false;
        }
        corners.resize(corners.size() / 3 * 3);
    }
    if (corners.empty()) { err = "STL file has no facets"; return false; }
    weldInto(corners, m);
    return true;
}

/* ---- PLY ---- */
enum class PlyType { I8, U8, I16, U16, I32, U32, F32, F64, Bad };

PlyType plyType(const std::string &s) {
    if (s == "char" || s == "int8") return PlyType::I8;
    if (s == "uchar" || s == "uint8") return PlyType::U8;
    if (s == "short" || s == "int16") return PlyType::I16;
    if (s == "ushort" || s == "uint16") return PlyType::U16;
    if (s == "int" || s == "int32") return PlyType::I32;
    if (s == "uint" || s == "uint32") return PlyType::U32;
    if (s == "float" || s == "float32") return PlyType::F32;
    if (s == "double" || s == "float64") return PlyType::F64;
    return PlyType::Bad;
}

size_t plySize(PlyType t) {
    switch (t) {
        case PlyType::I8: case PlyType::U8: return 1;
        case PlyType::I16: case PlyType::U16: return 2;
        case PlyType::I32: case PlyType::U32: case PlyType::F32: return 4;
        case PlyType::F64: return 8;
        default: return 0;
    }
}

struct PlyProp { std::string name; PlyType type = PlyType::Bad; bool list = false; PlyType countType = PlyType::Bad; };
struct PlyElem { std::string name; uint64_t count = 0; std::vector<PlyProp> props; };

/* reads values from the body, ascii or binary */
struct PlyReader {
    const std::string &data;
    size_t pos;
    int format; // 0 ascii, 1 little endian, 2 big endian
    bool ok = true;

    double read(PlyType t) {
        if (!ok) return 0.0;
        if (format == 0) {
            while (pos < data.size() && std::isspace((unsigned char)data[pos])) ++pos;
            if (pos >= data.size()) { ok = false; return 0.0; }
            const char *p = data.c_str() + pos;
            char *end;
            double d = std::strtod(p, &end);
            if (end == p) { ok = false; return 0.0; }
            pos += (size_t)(end - p);
            return d;
        }
        size_t n = plySize(t);
        if (n == 0 || pos + n > data.size()) { ok = false; return 0.0; }
        unsigned char b[8];
        std::memcpy(b, data.data() + pos, n);
        pos += n;
        bool hostLittle = std::endian::native == std::endian::little;
        if ((format == 1) != hostLittle) std::reverse(b, b + n);
        switch (t) {
            case PlyType::I8:  { int8_t x;   std::memcpy(&x, b, 1); return x; }
            case PlyType::U8:  { uint8_t x;  std::memcpy(&x, b, 1); return x; }
            case PlyType::I16: { int16_t x;  std::memcpy(&x, b, 2); return x; }
            case PlyType::U16: { uint16_t x; std::memcpy(&x, b, 2); return x; }
            case PlyType::I32: { int32_t x;  std::memcpy(&x, b, 4); return x; }
            case PlyType::U32: { uint32_t x; std::memcpy(&x, b, 4); return x; }
            case PlyType::F32: { float x;    std::memcpy(&x, b, 4); return x; }
            case PlyType::F64: { double x;   std::memcpy(&x, b, 8); return x; }
            default: ok = false; return 0.0;
        }
    }
};

bool loadPly(const std::string &data, RawMesh &m, std::string &err) {
    if (data.compare(0, 3, "ply") != 0) { err = "not a valid PLY file"; return false; }
    size_t hdrEnd = data.find("end_header");
    if (hdrEnd == std::string::npos) { err = "PLY header has no end_header"; return false; }
    size_t bodyStart = data.find('\n', hdrEnd);
    if (bodyStart == std::string::npos) { err = "PLY file has no data"; return false; }
    ++bodyStart;

    int format = -1;
    std::vector<PlyElem> elems;
    std::istringstream hdr(data.substr(0, hdrEnd));
    std::string line;
    while (std::getline(hdr, line)) {
        std::istringstream ls(line);
        std::string kw;
        ls >> kw;
        if (kw == "format") {
            std::string f;
            ls >> f;
            format = f == "ascii" ? 0 : f == "binary_little_endian" ? 1 : f == "binary_big_endian" ? 2 : -1;
            if (format < 0) { err = "unknown PLY format '" + f + "'"; return false; }
        } else if (kw == "element") {
            PlyElem e;
            ls >> e.name >> e.count;
            elems.push_back(e);
        } else if (kw == "property") {
            if (elems.empty()) { err = "PLY property before any element"; return false; }
            PlyProp p;
            std::string t;
            ls >> t;
            if (t == "list") {
                std::string ct, it;
                ls >> ct >> it >> p.name;
                p.list = true, p.countType = plyType(ct), p.type = plyType(it);
                if (p.countType == PlyType::Bad) { err = "bad PLY list type '" + ct + "'"; return false; }
            } else {
                p.type = plyType(t);
                ls >> p.name;
            }
            if (p.type == PlyType::Bad) { err = "bad PLY property type '" + t + "'"; return false; }
            elems.back().props.push_back(p);
        }
    }
    if (format < 0) { err = "PLY header has no format line"; return false; }

    PlyReader r{data, bodyStart, format};
    /* a sanity cap so a lying header can't make us allocate forever: every item takes >= 1 byte */
    const uint64_t maxItems = data.size();
    bool sawVerts = false;
    for (const PlyElem &e : elems) {
        if (e.count > maxItems) { err = "PLY element '" + e.name + "' count is too large"; return false; }
        int ix = -1, iy = -1, iz = -1, iface = -1;
        for (int i = 0; i < (int)e.props.size(); ++i) {
            const PlyProp &p = e.props[i];
            if (!p.list && p.name == "x") ix = i;
            if (!p.list && p.name == "y") iy = i;
            if (!p.list && p.name == "z") iz = i;
            if (p.list && (p.name == "vertex_indices" || p.name == "vertex_index" || iface < 0)) iface = i;
        }
        bool isVert = e.name == "vertex", isFace = e.name == "face";
        if (isVert) {
            if (ix < 0 || iy < 0 || iz < 0) { err = "PLY vertices have no x/y/z"; return false; }
            sawVerts = true;
            m.v.reserve((size_t)e.count);
        }
        std::vector<uint32_t> poly;
        for (uint64_t n = 0; n < e.count && r.ok; ++n) {
            glm::vec3 p(0.0f);
            for (int i = 0; i < (int)e.props.size() && r.ok; ++i) {
                const PlyProp &pr = e.props[i];
                if (pr.list) {
                    double c = r.read(pr.countType);
                    if (!r.ok || c < 0 || c > 1e6) { r.ok = false; break; }
                    poly.clear();
                    for (int k = 0; k < (int)c && r.ok; ++k) {
                        double idx = r.read(pr.type);
                        poly.push_back(idx < 0 || idx >= 4294967295.0 ? UINT32_MAX : (uint32_t)idx);
                    }
                    if (isFace && i == iface)
                        for (size_t k = 1; k + 1 < poly.size(); ++k) // fan-triangulate polygons
                            m.t.insert(m.t.end(), {poly[0], poly[k], poly[k + 1]});
                } else {
                    double d = r.read(pr.type);
                    if (isVert && i == ix) p.x = (float)d;
                    if (isVert && i == iy) p.y = (float)d;
                    if (isVert && i == iz) p.z = (float)d;
                }
            }
            if (isVert && r.ok) m.v.push_back(p);
        }
        if (!r.ok) { err = "PLY data ends early or is malformed (in '" + e.name + "')"; return false; }
    }
    if (!sawVerts) { err = "PLY file has no vertex element"; return false; }
    if (m.t.empty()) { err = "PLY file has no faces (point clouds can't be simulated)"; return false; }
    return true;
}

/* merge vertices into a uniform grid of cells, each cell's vertices become their average.
 * shrinks the grid until there are at most maxVerts cells */
void clusterDecimate(RawMesh &m, size_t maxVerts) {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const auto &p : m.v) lo = glm::min(lo, p), hi = glm::max(hi, p);
    float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-12f});

    double res = 512.0; // cells along the longest axis
    std::vector<uint32_t> cellOf(m.v.size());
    std::vector<glm::dvec3> sum;
    std::vector<uint32_t> cnt;
    for (int iter = 0; iter < 64; ++iter) {
        float h = extent / (float)res;
        std::unordered_map<uint64_t, uint32_t> cells;
        cells.reserve(std::min(m.v.size(), maxVerts * 4));
        sum.clear(), cnt.clear();
        for (size_t i = 0; i < m.v.size(); ++i) {
            glm::vec3 c = (m.v[i] - lo) / h;
            uint64_t key = (uint64_t)std::min(c.x, 2097151.0f)
                         | (uint64_t)std::min(c.y, 2097151.0f) << 21
                         | (uint64_t)std::min(c.z, 2097151.0f) << 42;
            auto [it, fresh] = cells.try_emplace(key, (uint32_t)sum.size());
            if (fresh) sum.emplace_back(0.0), cnt.push_back(0);
            sum[it->second] += glm::dvec3(m.v[i]);
            cnt[it->second]++;
            cellOf[i] = it->second;
        }
        if (sum.size() <= maxVerts) break;
        /* surfaces: cell count grows ~ res^2 */
        res *= std::min(0.95, 0.97 * std::sqrt((double)maxVerts / (double)sum.size()));
    }

    std::vector<glm::vec3> nv(sum.size());
    for (size_t c = 0; c < sum.size(); ++c) nv[c] = glm::vec3(sum[c] / (double)cnt[c]);
    std::vector<uint32_t> nt;
    nt.reserve(m.t.size());
    for (size_t i = 0; i + 2 < m.t.size(); i += 3) {
        uint32_t a = cellOf[m.t[i]], b = cellOf[m.t[i + 1]], c = cellOf[m.t[i + 2]];
        /* only drop collapsed triangles. never dedupe: two faces of a thin wall can land on the same
         * corners and voxelize() needs both hits to keep its in/out parity */
        if (a == b || b == c || a == c) continue;
        nt.insert(nt.end(), {a, b, c});
    }
    m.v = std::move(nv);
    m.t = std::move(nt);
}

bool finalizeMesh(RawMesh &m, TriMesh &out, std::string &err) {
    if (m.t.empty()) { err = "the file has no triangles"; return false; }
    for (uint32_t i : m.t)
        if (i >= m.v.size()) { err = "a face references a vertex that doesn't exist"; return false; }

    /* keep only referenced vertices */
    std::vector<uint32_t> remap(m.v.size(), UINT32_MAX);
    std::vector<glm::vec3> used;
    for (uint32_t &i : m.t) {
        if (remap[i] == UINT32_MAX) {
            const glm::vec3 &p = m.v[i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
                err = "the file has NaN/infinite vertex coordinates";
                return false;
            }
            remap[i] = (uint32_t)used.size();
            used.push_back(p);
        }
        i = remap[i];
    }
    m.v = std::move(used);

    /* indices are uint16_t, so anything bigger gets simplified to fit */
    if (m.v.size() > UINT16_MAX) {
        size_t before = m.v.size();
        clusterDecimate(m, UINT16_MAX);
        fprintf(stderr, "loadMesh: decimated %zu -> %zu vertices (%zu triangles)\n", before, m.v.size(), m.t.size() / 3);
        if (m.v.size() > UINT16_MAX) { err = "the mesh is too large to simplify"; return false; }
    }

    out.vertices = std::move(m.v);
    out.indices.reserve(m.t.size());
    for (size_t i = 0; i + 2 < m.t.size(); i += 3) {
        uint16_t a = (uint16_t)m.t[i], b = (uint16_t)m.t[i + 1], c = (uint16_t)m.t[i + 2];
        /* check if the triangle has area (degenerate) */
        glm::vec3 n = glm::cross(out.vertices[b] - out.vertices[a], out.vertices[c] - out.vertices[a]);
        if (glm::dot(n, n) < 1e-20f)
            continue;
        out.indices.insert(out.indices.end(), {a, b, c});
    }
    if (out.indices.empty()) { err = "every triangle is degenerate (zero area)"; return false; }

    /* compute our own normals since OBJ files don't normally include them */
    out.normals.assign(out.vertices.size(), glm::vec3{0.0f});
    for (size_t t = 0; t < out.indices.size(); t += 3) {
        uint16_t a = out.indices[t], b = out.indices[t + 1], c = out.indices[t + 2];
        glm::vec3 n = glm::cross(out.vertices[b] - out.vertices[a], out.vertices[c] - out.vertices[a]);
        out.normals[a] += n;
        out.normals[b] += n;
        out.normals[c] += n;
    }
    for (auto &n : out.normals)
        n = glm::length(n) > 0.0f ? glm::normalize(n) : glm::vec3(0, 1, 0);
    return true;
}

} // namespace

bool loadMesh(const std::string& path, TriMesh& out, std::string* errOut){
    out = {};
    std::string err;
    RawMesh raw;
    std::string ext = lowerExt(path);
    bool ok = false;
    try {
        if (ext == ".obj") {
            ok = loadObj(path, raw, err);
        } else if (ext == ".stl" || ext == ".ply") {
            std::string data;
            ok = readWholeFile(path, data, err) &&
                 (ext == ".stl" ? loadStl(data, raw, err) : loadPly(data, raw, err));
        } else {
            err = "unsupported file type '" + ext + "' (use .obj, .stl or .ply)";
        }
        ok = ok && finalizeMesh(raw, out, err);
    } catch (const std::bad_alloc &) {
        ok = false, err = "out of memory reading the mesh";
    }
    if (!ok) {
        out = {};
        fprintf(stderr, "loadMesh(%s): %s\n", path.c_str(), err.c_str());
        if (errOut) *errOut = err;
    }
    return ok;
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

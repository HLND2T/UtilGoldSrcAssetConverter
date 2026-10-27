#include "BSPConverterInternal.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <numeric>

namespace GoldSrcAssetConverter
{
constexpr double      s_PlaneTolerance      = 0.01;
constexpr double      s_NormalCosine        = 0.9999984769132877; //cos(0.1 degrees).
constexpr double      s_AreaEpsilon         = 1e-12;
constexpr double      s_TargetRelativeError = 0.05;
constexpr int         s_GridSize            = 128;
constexpr int         s_Subsamples          = 4;
constexpr std::size_t s_MaxLights           = 100000;

struct AreaVector
{
    double     x{}, y{}, z{};
    AreaVector operator+(AreaVector b) const
    {
        return {x + b.x, y + b.y, z + b.z};
    }
    AreaVector operator-(AreaVector b) const
    {
        return {x - b.x, y - b.y, z - b.z};
    }
    AreaVector operator*(double s) const
    {
        return {x * s, y * s, z * s};
    }
};

static double Dot(AreaVector a, AreaVector b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static AreaVector Cross(AreaVector a, AreaVector b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static AreaVector Unit(AreaVector a)
{
    return a * (1 / std::sqrt(Dot(a, a)));
}
static AreaVector Vector(const Vector3& a)
{
    return {a.x, a.y, a.z};
}
static nlohmann::json JSON(AreaVector a)
{
    return {a.x, a.y, a.z};
}

struct AreaTriangle
{
    AreaVector    p[3];
    Vector2       uv[3];
    AreaVector    normal;
    std::uint32_t face{};
};

//Positive-length collinear overlap also joins edges split by BSP T vertices.
static bool SharedEdge(const AreaTriangle& a, const AreaTriangle& b)
{
    if (Dot(a.normal, b.normal) < s_NormalCosine || std::abs(Dot(b.p[0] - a.p[0], a.normal)) > s_PlaneTolerance)
        return false;
    for (int i = 0; i < 3; ++i)
    {
        const auto d      = a.p[(i + 1) % 3] - a.p[i];
        const auto length = std::sqrt(Dot(d, d));
        if (length <= s_PlaneTolerance)
            continue;
        const auto axis = d * (1 / length);
        for (int j = 0; j < 3; ++j)
        {
            const auto v = b.p[j] - a.p[i];
            const auto w = b.p[(j + 1) % 3] - a.p[i];
            if (Dot(Cross(v, axis), Cross(v, axis)) > s_PlaneTolerance * s_PlaneTolerance ||
                Dot(Cross(w, axis), Cross(w, axis)) > s_PlaneTolerance * s_PlaneTolerance)
                continue;
            const double t0 = Dot(v, axis), t1 = Dot(w, axis);
            if (std::min(length, std::max(t0, t1)) - std::max(0.0, std::min(t0, t1)) > s_PlaneTolerance)
                return true;
        }
    }
    return false;
}

struct AreaPoint
{
    double x{}, y{};
};
static double Turn(AreaPoint a, AreaPoint b, AreaPoint c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

//Monotone-chain hull followed by edge-oriented bounding boxes. Ties retain hull order.
static void FitAxes(const std::vector<AreaTriangle>& triangles, const std::vector<std::size_t>& group, AreaVector& u, AreaVector& v, double& minX, double& minY, double& maxX, double& maxY)
{
    const auto             n        = triangles[group[0]].normal;
    const auto             initialU = Unit(triangles[group[0]].p[1] - triangles[group[0]].p[0]);
    const auto             initialV = Cross(n, initialU);
    std::vector<AreaPoint> points;
    for (auto i : group)
        for (auto p : triangles[i].p)
            points.push_back({Dot(p, initialU), Dot(p, initialV)});
    std::sort(points.begin(), points.end(), [](auto a, auto b) { return std::tie(a.x, a.y) < std::tie(b.x, b.y); });
    points.erase(std::unique(points.begin(), points.end(), [](auto a, auto b) { return a.x == b.x && a.y == b.y; }), points.end());
    std::vector<AreaPoint> hull(points.size() * 2);
    std::size_t            count = 0;
    for (auto p : points)
    {
        while (count >= 2 && Turn(hull[count - 2], hull[count - 1], p) <= 0) --count;
        hull[count++] = p;
    }
    const auto lower = count;
    for (std::size_t i = points.size() - 1; i > 0; --i)
    {
        const auto p = points[i - 1];
        while (count > lower && Turn(hull[count - 2], hull[count - 1], p) <= 0) --count;
        hull[count++] = p;
    }
    hull.resize(count - 1);
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < hull.size(); ++i)
    {
        const auto a = hull[i], b = hull[(i + 1) % hull.size()];
        const auto axisU = Unit(initialU * (b.x - a.x) + initialV * (b.y - a.y));
        const auto axisV = Cross(n, axisU);
        double     x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
        for (auto j : group)
            for (auto p : triangles[j].p)
            {
                x0 = std::min(x0, Dot(p, axisU));
                x1 = std::max(x1, Dot(p, axisU));
                y0 = std::min(y0, Dot(p, axisV));
                y1 = std::max(y1, Dot(p, axisV));
            }
        const auto area = (x1 - x0) * (y1 - y0);
        if (area < best)
        {
            best = area;
            u    = axisU;
            v    = axisV;
            minX = x0;
            maxX = x1;
            minY = y0;
            maxY = y1;
        }
    }
}

static double Linear(double s)
{
    return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

//Bilinear level-zero sampling, with glTF's repeat wrapping and sRGB decode before filtering.
static AreaVector Sample(const BSPDecodedImage& image, double u, double v, bool bMask, double cutoff)
{
    const double x  = (u - std::floor(u)) * image.width - 0.5;
    const double y  = (v - std::floor(v)) * image.height - 0.5;
    const int    ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    AreaVector   rgb;
    double       alpha = 0;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
        {
            const int    px     = (ix + dx + static_cast<int>(image.width)) % image.width;
            const int    py     = (iy + dy + static_cast<int>(image.height)) % image.height;
            const auto*  p      = image.texels.data() + (static_cast<std::size_t>(py) * image.width + px) * 4;
            const double weight = (dx ? x - ix : 1 - (x - ix)) * (dy ? y - iy : 1 - (y - iy));
            rgb                 = rgb + AreaVector{Linear(p[0] / 255.0), Linear(p[1] / 255.0), Linear(p[2] / 255.0)} * weight;
            alpha += p[3] / 255.0 * weight;
        }
    return bMask && alpha < cutoff ? AreaVector{} : rgb;
}

struct AreaRect
{ int x0{}, y0{}, x1{}, y1{}; };
struct AreaMoments
{
    AreaVector  sum;
    double      squared{};
    AreaMoments operator+(AreaMoments b) const
    {
        return {sum + b.sum, squared + b.squared};
    }
    AreaMoments operator-(AreaMoments b) const
    {
        return {sum - b.sum, squared - b.squared};
    }
};

class CAreaGrid
{
public:
    CAreaGrid(int width, int height) : m_Width(width), m_Height(height), m_Prefix((width + 1) * (height + 1)) {}
    void Add(int x, int y, AreaVector value)
    {
        m_Prefix[(y + 1) * (m_Width + 1) + x + 1] = AreaMoments{value, Dot(value, value)} +
            m_Prefix[y * (m_Width + 1) + x + 1] + m_Prefix[(y + 1) * (m_Width + 1) + x] - m_Prefix[y * (m_Width + 1) + x];
    }
    AreaMoments Moments(AreaRect r) const
    {
        return m_Prefix[r.y1 * (m_Width + 1) + r.x1] - m_Prefix[r.y0 * (m_Width + 1) + r.x1] -
            m_Prefix[r.y1 * (m_Width + 1) + r.x0] + m_Prefix[r.y0 * (m_Width + 1) + r.x0];
    }
    double Error(AreaRect r) const
    {
        const auto m = Moments(r);
        return std::max(0.0, m.squared - Dot(m.sum, m.sum) / ((r.x1 - r.x0) * (r.y1 - r.y0)));
    }
    AreaRect Trim(AreaRect r) const
    {
        const auto empty = [&](AreaRect q) {
            const auto s = Moments(q).sum;
            return s.x + s.y + s.z <= 0;
        };
        while (r.x0 < r.x1 && empty({r.x0, r.y0, r.x0 + 1, r.y1})) ++r.x0;
        while (r.x0 < r.x1 && empty({r.x1 - 1, r.y0, r.x1, r.y1})) --r.x1;
        while (r.y0 < r.y1 && empty({r.x0, r.y0, r.x1, r.y0 + 1})) ++r.y0;
        while (r.y0 < r.y1 && empty({r.x0, r.y1 - 1, r.x1, r.y1})) --r.y1;
        return r;
    }
    int m_Width{}, m_Height{};

private:
    std::vector<AreaMoments> m_Prefix;
};

static bool Nonempty(AreaRect r)
{
    return r.x1 > r.x0 && r.y1 > r.y0;
}

static std::vector<AreaRect> Partition(const CAreaGrid& grid, std::uint32_t budget, double& relativeError)
{
    const auto initial = grid.Trim({0, 0, grid.m_Width, grid.m_Height});
    if (!Nonempty(initial))
    {
        relativeError = 0;
        return {};
    }
    const double          energy = grid.Moments(initial).squared;
    const double          target = energy * s_TargetRelativeError * s_TargetRelativeError;
    std::vector<AreaRect> rects{initial};
    double                total = grid.Error(initial);
    while (rects.size() < budget && total > target)
    {
        double      bestGain  = 0;
        std::size_t bestIndex = 0;
        AreaRect    bestA, bestB;
        for (std::size_t i = 0; i < rects.size(); ++i)
        {
            const auto r = rects[i];
            for (int axis = 0; axis < 2; ++axis)
                for (int cut = (axis ? r.y0 : r.x0) + 1; cut < (axis ? r.y1 : r.x1); ++cut)
                {
                    auto a = r, b = r;
                    if (axis)
                    {
                        a.y1 = cut;
                        b.y0 = cut;
                    }
                    else
                    {
                        a.x1 = cut;
                        b.x0 = cut;
                    }
                    a                 = grid.Trim(a);
                    b                 = grid.Trim(b);
                    const double gain = grid.Error(r) - (Nonempty(a) ? grid.Error(a) : 0) - (Nonempty(b) ? grid.Error(b) : 0);
                    if (gain > bestGain)
                    {
                        bestGain  = gain;
                        bestIndex = i;
                        bestA     = a;
                        bestB     = b;
                    }
                }
        }
        if (bestGain <= s_AreaEpsilon) break;
        rects.erase(rects.begin() + bestIndex);
        if (Nonempty(bestA)) rects.push_back(bestA);
        if (Nonempty(bestB)) rects.push_back(bestB);
        total = std::max(0.0, total - bestGain);
    }
    //Only merge boxes whose union is rectangular, so disconnected dark gaps are never bridged.
    for (std::size_t i = 0; i < rects.size(); ++i)
        for (std::size_t j = i + 1; j < rects.size();)
        {
            const auto a = rects[i], b = rects[j];
            const bool adjacent = (a.x0 == b.x0 && a.x1 == b.x1 && (a.y1 == b.y0 || b.y1 == a.y0)) ||
                (a.y0 == b.y0 && a.y1 == b.y1 && (a.x1 == b.x0 || b.x1 == a.x0));
            const AreaRect merged{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
            const double   next = total - grid.Error(a) - grid.Error(b) + grid.Error(merged);
            if (adjacent && next <= target)
            {
                rects[i] = merged;
                rects.erase(rects.begin() + j);
                total = next;
            }
            else
                ++j;
        }
    relativeError = energy > 0 ? std::sqrt(std::max(0.0, total) / energy) : 0;
    return rects;
}

using AreaMatrix = std::array<double, 16>;
static AreaMatrix Identity()
{
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
static AreaMatrix Multiply(const AreaMatrix& a, const AreaMatrix& b)
{
    AreaMatrix c{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                c[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
    return c;
}
static AreaVector Transform(const AreaMatrix& m, AreaVector p, double w)
{
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12] * w, m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13] * w, m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] * w};
}
static AreaMatrix NodeMatrix(const tinygltf::Node& node)
{
    auto m = Identity();
    if (node.matrix.size() == 16)
    {
        std::copy(node.matrix.begin(), node.matrix.end(), m.begin());
        return m;
    }
    if (node.rotation.size() == 4)
    {
        const double x = node.rotation[0], y = node.rotation[1], z = node.rotation[2], w = node.rotation[3];
        m = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0, 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
             2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0, 0, 0, 0, 1};
    }
    if (node.scale.size() == 3)
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row) m[col * 4 + row] *= node.scale[col];
    if (node.translation.size() == 3)
        for (int i = 0; i < 3; ++i) m[12 + i] = node.translation[i];
    return m;
}
static nlohmann::json Quaternion(AreaVector x, AreaVector y, AreaVector z)
{
    double       qx, qy, qz, qw;
    const double trace = x.x + y.y + z.z;
    if (trace > 0)
    {
        const double s = std::sqrt(trace + 1) * 2;
        qw             = s / 4;
        qx             = (y.z - z.y) / s;
        qy             = (z.x - x.z) / s;
        qz             = (x.y - y.x) / s;
    }
    else if (x.x > y.y && x.x > z.z)
    {
        const double s = std::sqrt(1 + x.x - y.y - z.z) * 2;
        qw             = (y.z - z.y) / s;
        qx             = s / 4;
        qy             = (y.x + x.y) / s;
        qz             = (z.x + x.z) / s;
    }
    else if (y.y > z.z)
    {
        const double s = std::sqrt(1 + y.y - x.x - z.z) * 2;
        qw             = (z.x - x.z) / s;
        qx             = (y.x + x.y) / s;
        qy             = s / 4;
        qz             = (z.y + y.z) / s;
    }
    else
    {
        const double s = std::sqrt(1 + z.z - x.x - y.y) * 2;
        qw             = (x.y - y.x) / s;
        qx             = (z.x + x.z) / s;
        qy             = (z.y + y.z) / s;
        qz             = s / 4;
    }
    return {qx, qy, qz, qw};
}

nlohmann::json FitBSPAreaLightPrimitive(const BSPPrimitive& primitive, const tinygltf::Material& material, const BSPDecodedImage& image, std::uint32_t budget, nlohmann::json& stats, std::uint32_t modelIndex)
{
    std::vector<AreaTriangle> triangles;
    for (const auto& face : primitive.faces)
    {
        bool bValid = false;
        for (std::size_t i = face.firstIndex; i + 2 < face.firstIndex + face.indexCount; i += 3)
        {
            AreaTriangle t;
            for (int j = 0; j < 3; ++j)
            {
                const auto index = primitive.indices[i + j];
                t.p[j]           = Vector(primitive.positions[index]);
                t.uv[j]          = primitive.texcoords[index];
            }
            const auto n = Cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
            if (Dot(n, n) <= s_AreaEpsilon) continue;
            t.normal = Unit(n);
            t.face   = face.face;
            triangles.push_back(t);
            bValid = true;
        }
        if (bValid) stats["emissiveFaceCount"] = stats["emissiveFaceCount"].get<std::size_t>() + 1;
    }
    std::vector<std::size_t> roots(triangles.size());
    std::iota(roots.begin(), roots.end(), 0);
    const auto root = [&](std::size_t i) {
        while (roots[i] != i) i = roots[i];
        return i;
    };
    for (std::size_t i = 0; i < triangles.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (root(i) != root(j) && SharedEdge(triangles[i], triangles[j])) roots[root(i)] = root(j);
    std::map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < triangles.size(); ++i) groups[root(i)].push_back(i);
    nlohmann::json   lights   = nlohmann::json::array();
    const auto       strength = material.extensions.at("KHR_materials_emissive_strength").Get("emissiveStrength").GetNumberAsDouble();
    const AreaVector factor{material.emissiveFactor[0] * strength, material.emissiveFactor[1] * strength, material.emissiveFactor[2] * strength};
    for (const auto& entry : groups)
    {
        const auto& group   = entry.second;
        const auto  groupID = stats["groupCount"].get<std::size_t>();
        stats["groupCount"] = groupID + 1;
        AreaVector u, v;
        double     x0{}, y0{}, x1{}, y1{};
        FitAxes(triangles, group, u, v, x0, y0, x1, y1);
        const double width = x1 - x0, height = y1 - y0;
        if (width * height <= s_AreaEpsilon) continue;
        const int  nx = std::max(1, static_cast<int>(std::ceil(s_GridSize * width / std::max(width, height))));
        const int  ny = std::max(1, static_cast<int>(std::ceil(s_GridSize * height / std::max(width, height))));
        CAreaGrid  grid(nx, ny);
        const auto normal = triangles[group[0]].normal;
        const auto plane  = normal * Dot(normal, triangles[group[0]].p[0]);
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
            {
                AreaVector value;
                for (int sy = 0; sy < s_Subsamples; ++sy)
                    for (int sx = 0; sx < s_Subsamples; ++sx)
                    {
                        const auto p = plane + u * (x0 + (x + (sx + 0.5) / s_Subsamples) * width / nx) + v * (y0 + (y + (sy + 0.5) / s_Subsamples) * height / ny);
                        for (auto index : group)
                        {
                            const auto&  t = triangles[index];
                            const auto   a = t.p[1] - t.p[0], b = t.p[2] - t.p[0], c = p - t.p[0];
                            const double aa = Dot(a, a), ab = Dot(a, b), bb = Dot(b, b), ca = Dot(c, a), cb = Dot(c, b), denom = aa * bb - ab * ab;
                            const double s = (bb * ca - ab * cb) / denom, r = (aa * cb - ab * ca) / denom;
                            if (s < -s_AreaEpsilon || r < -s_AreaEpsilon || s + r > 1 + s_AreaEpsilon) continue;
                            const auto color = Sample(image, t.uv[0].x * (1 - s - r) + t.uv[1].x * s + t.uv[2].x * r, t.uv[0].y * (1 - s - r) + t.uv[1].y * s + t.uv[2].y * r, material.alphaMode == "MASK", material.alphaCutoff);
                            value            = value + AreaVector{color.x * factor.x, color.y * factor.y, color.z * factor.z} * (1.0 / (s_Subsamples * s_Subsamples));
                            break;
                        }
                    }
                grid.Add(x, y, value);
            }
        double     relativeError{};
        const auto rects = Partition(grid, budget, relativeError);
        if (relativeError > s_TargetRelativeError)
            stats["limitedGroups"].push_back({{"group", groupID}, {"modelIndex", modelIndex}, {"relativeError", relativeError}});
        std::vector<std::uint32_t> faces;
        for (auto i : group) faces.push_back(triangles[i].face);
        std::sort(faces.begin(), faces.end());
        faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
        for (auto rect : rects)
        {
            const auto rgb = grid.Moments(rect).sum * (1.0 / ((rect.x1 - rect.x0) * (rect.y1 - rect.y0)));
            lights.push_back({{"position", JSON(plane + u * (x0 + (rect.x0 + rect.x1) * 0.5 * width / nx) + v * (y0 + (rect.y0 + rect.y1) * 0.5 * height / ny))},
                              {"axisX", JSON(u)},
                              {"axisY", JSON(v * -1)},
                              {"width", (rect.x1 - rect.x0) * width / nx},
                              {"height", (rect.y1 - rect.y0) * height / ny},
                              {"radianceRGB", JSON(rgb)},
                              {"faceIndices", faces},
                              {"materialIndex", primitive.material},
                              {"group", groupID}});
        }
    }
    return lights;
}

ConversionError BuildBSPAreaLights(const BSPConversionSource& source, const std::vector<BSPModelGeometry>& models, const CBSPTextureConverter& textures, const tinygltf::Model& gltf, nlohmann::json& output, std::string& error)
{
    const auto& options = source.pRequest->options;
    output              = {{"schemaVersion", 1}, {"coordinateSystem", "GLTF_Y_UP"}, {"unitScale", options.scaleFactor}, {"enabled", options.bExportAreaLights}, {"lights", nlohmann::json::array()}, {"statistics", {{"emissiveFaceCount", 0}, {"groupCount", 0}, {"rectangleCount", 0}, {"limitedGroups", nlohmann::json::array()}}}};
    if (!options.bExportAreaLights) return ConversionError::None;
    std::map<int, std::pair<std::uint32_t, nlohmann::json>> meshLights;
    //Only extract meshes actually referenced by the exported scene.
    std::vector<AreaMatrix> matrices(gltf.nodes.size());
    std::vector<bool>       visited(gltf.nodes.size());
    const auto              visit = [&](auto&& self, int index, const AreaMatrix& parent) -> void {
        visited[index]  = true;
        matrices[index] = Multiply(parent, NodeMatrix(gltf.nodes[index]));
        for (int child : gltf.nodes[index].children) self(self, child, matrices[index]);
    };
    for (int index : gltf.scenes[gltf.defaultScene].nodes) visit(visit, index, Identity());
    for (const auto& model : models)
    {
        const int mesh = model.meshes[static_cast<std::size_t>(BSPGeometryKind::Surface)];
        if (mesh < 0) continue;
        bool bUsed = false;
        for (std::size_t i = 0; i < gltf.nodes.size(); ++i)
            if (visited[i] && gltf.nodes[i].mesh == mesh) bUsed = true;
        if (!bUsed) continue;
        auto local = nlohmann::json::array();
        for (const auto& primitive : model.primitives[static_cast<std::size_t>(BSPGeometryKind::Surface)])
        {
            const auto& material = gltf.materials[primitive.material];
            if (material.emissiveTexture.index < 0 || material.emissiveFactor.size() != 3 || !material.extensions.count("KHR_materials_emissive_strength")) continue;
            const auto* pImage = textures.GetDecodedImage(material.emissiveTexture.index);
            if (!pImage) continue;
            auto name = material.name;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.find("aaatrigger") != std::string::npos) continue;
            auto extracted = FitBSPAreaLightPrimitive(primitive, material, *pImage, options.maxAreaLightsPerGroup, output["statistics"], model.model);
            for (auto& light : extracted) local.push_back(std::move(light));
        }
        meshLights.emplace(mesh, std::make_pair(model.model, std::move(local)));
    }
    for (std::size_t i = 0; i < gltf.nodes.size(); ++i)
    {
        if (!visited[i]) continue;
        const auto found = meshLights.find(gltf.nodes[i].mesh);
        if (found == meshLights.end()) continue;
        int entity = -1;
        //Mesh nodes are children of the entity/world node.
        for (const auto& parent : gltf.nodes)
            if (std::find(parent.children.begin(), parent.children.end(), static_cast<int>(i)) != parent.children.end() && parent.extras.Has("goldsrc"))
            {
                const auto& extras = parent.extras.Get("goldsrc");
                if (extras.Has("entityIndex")) entity = extras.Get("entityIndex").GetNumberAsInt();
            }
        for (const auto& local : found->second.second)
        {
            if (output["lights"].size() >= s_MaxLights)
            {
                error = "area light count exceeds 100000";
                return ConversionError::LimitExceeded;
            }
            auto       light = local;
            const auto vec   = [](const nlohmann::json& j) -> AreaVector {
                return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
            };
            const auto& m = matrices[i];
            const auto  x = Transform(m, vec(light["axisX"]), 0), y = Transform(m, vec(light["axisY"]), 0);
            const auto  ux = Unit(x), uy = Unit(y);
            light["position"]   = JSON(Transform(m, vec(light["position"]), 1));
            light["quaternion"] = Quaternion(ux, uy, Cross(ux, uy));
            light["width"]      = light["width"].get<double>() * std::sqrt(Dot(x, x));
            light["height"]     = light["height"].get<double>() * std::sqrt(Dot(y, y));
            light.erase("axisX");
            light.erase("axisY");
            light["entityIndex"] = entity;
            light["modelIndex"]  = found->second.first;
            light["id"]          = "node" + std::to_string(i) + "-group" + std::to_string(light["group"].get<std::size_t>()) + "-rect" + std::to_string(output["lights"].size());
            for (const auto* field : {"position", "quaternion", "radianceRGB"})
                for (const auto& value : light[field])
                    if (!std::isfinite(value.get<double>()) || std::abs(value.get<double>()) > std::numeric_limits<float>::max())
                    {
                        error = "nonfinite or overflowing area light";
                        return ConversionError::InvalidRequest;
                    }
            for (const auto* field : {"width", "height"})
                if (!std::isfinite(light[field].get<double>()) || light[field].get<double>() <= 0 || light[field].get<double>() > std::numeric_limits<float>::max())
                {
                    error = "invalid area light dimensions";
                    return ConversionError::InvalidRequest;
                }
            output["lights"].push_back(std::move(light));
        }
    }
    output["statistics"]["rectangleCount"] = output["lights"].size();
    return ConversionError::None;
}
} // namespace GoldSrcAssetConverter

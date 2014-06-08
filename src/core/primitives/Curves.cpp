#include "Curves.hpp"
#include "Mesh.hpp"

#include "math/TangentFrame.hpp"
#include "math/MathUtil.hpp"
#include "math/Vec.hpp"

#include "io/FileUtils.hpp"

#include <fstream>

namespace Tungsten
{

class Scene;

// http://www.answers.com/topic/b-spline
template<typename T>
inline T quadraticBSpline(T p0, T p1, T p2, float t)
{
    return (0.5f*p0 - p1 + 0.5f*p2)*t*t + (p1 - p0)*t + 0.5f*(p0 + p1);
}

template<typename T>
inline T quadraticBSplineDeriv(T p0, T p1, T p2, float t)
{
    return (p0 - 2.0f*p1 + p2)*t + (p1 - p0);
}

inline Vec2f minMaxQuadratic(float p0, float p1, float p2)
{
    float xMin = (p0 + p1)*0.5f;
    float xMax = (p1 + p2)*0.5f;
    if (xMin > xMax)
        std::swap(xMin, xMax);

    float tFlat = (p0 - p1)/(p0 - 2.0f*p1 + p2);
    if (tFlat > 0.0f && tFlat < 1.0f) {
        float xFlat = quadraticBSpline(p0, p1, p2, tFlat);
        xMin = min(xMin, xFlat);
        xMax = max(xMax, xFlat);
    }
    return Vec2f(xMin, xMax);
}

static bool pointOnSpline(const Vec4f &q0, const Vec4f &q1, const Vec4f &q2,
        float tMin, float tMax, float &t, Vec2f &uv, float &width)
{
    constexpr int MaxDepth = 5;

    struct StackNode
    {
        Vec2f p0, p1;
        float w0, w1;
        float tMin, tSpan;
        int depth;

        void set(float tMin_, float tSpan_, Vec2f p0_, Vec2f p1_, float w0_, float w1_, int depth_)
        {
            p0 = p0_;
            p1 = p1_;
            w0 = w0_;
            w1 = w1_;
            tMin = tMin_;
            tSpan = tSpan_;
            depth = depth_;
        }
    };

    Vec2f p0 = q0.xy(), p1 = q1.xy(), p2 = q2.xy();
    Vec2f tFlat = (p0 - p1)/(p0 - 2.0f*p1 + p2);
    float xFlat = quadraticBSpline(p0.x(), p1.x(), p2.x(), tFlat.x());
    float yFlat = quadraticBSpline(p0.y(), p1.y(), p2.y(), tFlat.y());

    Vec2f deriv1(p0 - 2.0f*p1 + p2), deriv2(p1 - p0);

    StackNode stackBuf[MaxDepth];
    const StackNode *top = &stackBuf[0];
    StackNode *stack = &stackBuf[0];

    StackNode cur{(p0 + p1)*0.5f, (p1 + p2)*0.5f, (q0.w() + q1.w())*0.5f, (q1.w() + q2.w())*0.5f, 0.0f, 1.0f, 0};

    float closestDepth = tMax;

    while (true) {
        Vec2f pMin = min(cur.p0, cur.p1);
        Vec2f pMax = max(cur.p0, cur.p1);
        if (tFlat.x() > cur.tMin && tFlat.x() < cur.tMin + cur.tSpan) {
            pMin.x() = min(pMin.x(), xFlat);
            pMax.x() = max(pMax.x(), xFlat);
        }
        if (tFlat.y() > cur.tMin && tFlat.y() < cur.tMin + cur.tSpan) {
            pMin.y() = min(pMin.y(), yFlat);
            pMax.y() = max(pMax.y(), yFlat);
        }

        float testWidth = max(cur.w0, cur.w1);
        if (pMin.x() <= testWidth && pMin.y() <= testWidth && pMax.x() >= -testWidth && pMax.y() >= -testWidth) {
            if (cur.depth >= MaxDepth) {
                Vec2f tangent0 = deriv2 + deriv1*cur.tMin;
                Vec2f tangent1 = deriv2 + deriv1*(cur.tMin + cur.tSpan);

                if (tangent0.dot(cur.p0) <= 0.0f && tangent1.dot(cur.p1) >= 0.0f) {
                    Vec2f v = (cur.p1 - cur.p0);
                    float lengthSq = v.lengthSq();
                    float segmentT = -(cur.p0.dot(v))/lengthSq;
                    float distance;
                    float signedUnnormalized = cur.p0.x()*v.y() - cur.p0.y()*v.x();
                    if (segmentT <= 0.0f)
                        distance = cur.p0.length();
                    else if (segmentT >= 1.0f)
                        distance = cur.p1.length();
                    else
                        distance = std::fabs(signedUnnormalized)/std::sqrt(lengthSq);

                    float newT = segmentT*cur.tSpan + cur.tMin;
                    float currentWidth = quadraticBSpline(q0.w(), q1.w(), q2.w(), newT);
                    float currentDepth = quadraticBSpline(q0.z(), q1.z(), q2.z(), newT);
                    if (currentDepth < tMax && currentDepth > tMin && distance < currentWidth && newT >= 0.0f && newT <= 1.0f) {
                        float halfDistance = 0.5f*distance/currentWidth;
                        uv = Vec2f(newT, signedUnnormalized < 0.0f ? 0.5f - halfDistance : halfDistance + 0.5f);
                        t = currentDepth;
                        width = currentWidth;
                        closestDepth = currentDepth;
                    }
                }
            } else {
                float newSpan = cur.tSpan*0.5f;
                float splitT = cur.tMin + newSpan;
                Vec4f qSplit = quadraticBSpline(q0, q1, q2, splitT);
                (*stack++).set(cur.tMin, newSpan, cur.p0, qSplit.xy(), cur.w0, qSplit.w(), cur.depth + 1);
                cur.set(splitT, newSpan, qSplit.xy(), cur.p1, qSplit.w(), cur.w1, cur.depth + 1);
                continue;
            }
        }
        if (stack == top)
            break;
        cur = *--stack;
    }

    return closestDepth < tMax;
}

static Vec4f project(const Vec3f &o, const Vec3f &lx, const Vec3f &ly, const Vec3f &lz, const Vec4f &q)
{
    Vec3f p(q.xyz() - o);
    return Vec4f(lx.dot(p), ly.dot(p), lz.dot(p), q.w());
}

static Box3f curveBox(const Vec4f &q0, const Vec4f &q1, const Vec4f &q2)
{
    Vec2f xMinMax(minMaxQuadratic(q0.x(), q1.x(), q2.x()));
    Vec2f yMinMax(minMaxQuadratic(q0.y(), q1.y(), q2.y()));
    Vec2f zMinMax(minMaxQuadratic(q0.z(), q1.z(), q2.z()));
    float maxW = max(q0.w(), q1.w(), q2.w());
    return Box3f(
        Vec3f(xMinMax.x(), yMinMax.x(), zMinMax.x()) - maxW,
        Vec3f(xMinMax.y(), yMinMax.y(), zMinMax.y()) + maxW
    );
}

void Curves::loadCurves()
{
    std::ifstream in(_path, std::ios_base::in | std::ios_base::binary);

    ASSERT(in.good(), "Unable to open curve file at '%s'", _path);

    char magic[5];
    in.get(magic, 5);
    ASSERT(std::string(magic) == "HAIR", "Error while loading curves: "
            "Missing 'HAIR' identifier at beginning of file '%s'", _path);
    FileUtils::streamRead(in, _curveCount);
    FileUtils::streamRead(in, _nodeCount);
    uint32 descriptor;
    FileUtils::streamRead(in, descriptor);

    bool hasSegments     = descriptor & 0x01;
    bool hasPoints       = descriptor & 0x02;
    bool hasThickness    = descriptor & 0x04;
    bool hasTransparency = descriptor & 0x08;
    bool hasColor        = descriptor & 0x10;

    uint32 defaultSegments;
    float defaultThickness;
    float defaultTransparency;
    Vec3f defaultColor;
    FileUtils::streamRead(in, defaultSegments);
    FileUtils::streamRead(in, defaultThickness);
    FileUtils::streamRead(in, defaultTransparency);
    FileUtils::streamRead(in, defaultColor);

    char fileInfo[89];
    in.read(fileInfo, 88);
    fileInfo[88] = '\0';
    DBG("File info for '%s': '%s'", _path, fileInfo);

    _curveEnds.resize(_curveCount);
    if (hasSegments) {
        std::vector<uint16> segmentLength(_curveCount);
        FileUtils::streamRead(in, segmentLength);
        for (size_t i = 0; i < _curveCount; ++i)
            _curveEnds[i] = segmentLength[i] + 1 + (i > 0 ? _curveEnds[i - 1] : 0);
    } else {
        for (size_t i = 0; i < _curveCount; ++i)
            _curveEnds[i] = (i + 1)*defaultSegments;
    }

    ASSERT(hasPoints,  "Error while loading curves: "
            "Missing points array in file '%s'", _path);
    std::vector<Vec3f> points(_nodeCount);
    FileUtils::streamRead(in, points);
    _nodeData.resize(_nodeCount);
    for (size_t i = 0; i < _nodeCount; ++i)
        _nodeData[i] = Vec4f(points[i].x(), points[i].y(), points[i].z(), defaultThickness);

    if (hasThickness) {
        std::vector<float> thicknesses(_nodeCount);
        FileUtils::streamRead(in, thicknesses);
        for (size_t i = 0; i < _nodeCount; ++i)
            _nodeData[i].w() = thicknesses[i];
    }

    if (hasTransparency)
        in.seekg(sizeof(float)*_nodeCount, std::ios_base::cur);

    if (hasColor) {
        _nodeColor.resize(_nodeCount);
        FileUtils::streamRead(in, _nodeColor);
    } else {
        _nodeColor.clear();
        _nodeColor.push_back(defaultColor);
    }

    _curveEnds.shrink_to_fit();
    _nodeData.shrink_to_fit();
    _nodeColor.shrink_to_fit();
}

void Curves::computeBounds()
{
    _bounds = Box3f();
    for (size_t i = 2; i < _nodeData.size(); ++i)
        _bounds.grow(curveBox(_nodeData[i - 2], _nodeData[i - 1], _nodeData[i]));
}

void Curves::fromJson(const rapidjson::Value &v, const Scene &scene)
{
    Primitive::fromJson(v, scene);
    _path = JsonUtils::as<std::string>(v, "file");

    loadCurves();
}

rapidjson::Value Curves::toJson(Allocator &allocator) const
{
    rapidjson::Value v = Primitive::toJson(allocator);
    v.AddMember("type", "curves", allocator);
    v.AddMember("file", _path.c_str(), allocator);
    return std::move(v);
}

bool Curves::intersect(Ray &ray, IntersectionTemporary &data) const
{
    Vec3f o(ray.pos()), lz(ray.dir());
    float d = std::sqrt(lz.x()*lz.x() + lz.z()*lz.z());
    Vec3f lx, ly;
    if (d == 0.0f) {
        lx = Vec3f(1.0f, 0.0f, 0.0f);
        ly = Vec3f(0.0f, 0.0f, -lz.y());
    } else {
        lx = Vec3f(lz.z()/d, 0.0f, -lz.x()/d);
        ly = Vec3f(lx.z()*lz.y(), d, -lz.y()*lx.x());
    }

    bool didIntersect = false;
    CurveIntersection &isect = *data.as<CurveIntersection>();

    _bvh->trace(ray, [&](Ray &ray, uint32 id) {
        Vec4f q0(project(o, lx, ly, lz, _nodeData[id - 2]));
        Vec4f q1(project(o, lx, ly, lz, _nodeData[id - 1]));
        Vec4f q2(project(o, lx, ly, lz, _nodeData[id - 0]));

        if (pointOnSpline(q0, q1, q2, ray.nearT(), ray.farT(), isect.t, isect.uv, isect.w)) {
            ray.setFarT(isect.t);
            didIntersect = true;
        }
    });

    if (didIntersect)
        data.primitive = this;

    return didIntersect;
}

void Curves::buildProxy()
{
    std::vector<Vertex> verts;
    std::vector<TriangleI> tris;

    const int Samples = _curveCount < 100 ? 100 : (_curveCount < 10000 ? 5 : 2);

    uint32 idx = 0;
    for (uint32 i = 0; i < _curveCount; ++i) {
        uint32 start = 0;
        if (i > 0)
            start = _curveEnds[i - 1];

        for (uint32 t = start + 2; t < _curveEnds[i]; ++t) {
            const Vec4f &p0 = _nodeData[t - 2];
            const Vec4f &p1 = _nodeData[t - 1];
            const Vec4f &p2 = _nodeData[t - 0];

            for (int j = 0; j <= Samples; ++j) {
                float curveT = j*(1.0f/Samples);
                Vec3f tangent = quadraticBSplineDeriv(p0.xyz(), p1.xyz(), p2.xyz(), curveT).normalized();
                TangentFrame frame(tangent);
                Vec4f p = quadraticBSpline(p0, p1, p2, curveT);
                Vec3f v0 = frame.toGlobal(Vec3f(-p.w(), 0.0f, 0.0f)) + p.xyz();
                Vec3f v1 = frame.toGlobal(Vec3f( p.w(), 0.0f, 0.0f)) + p.xyz();

                verts.emplace_back(v0);
                verts.emplace_back(v1);
                idx += 2;
                if (j > 0) {
                    tris.emplace_back(idx - 3, idx - 2, idx - 1);
                    tris.emplace_back(idx - 4, idx - 2, idx - 3);
                }
            }
        }
    }

    _proxy = std::make_shared<TriangleMesh>(verts, tris, _bsdf, "Curves", false);
}

void Curves::intersectionInfo(const IntersectionTemporary &data, IntersectionInfo &info) const
{
    const CurveIntersection &isect = *data.as<CurveIntersection>();
    info.Ng = info.Ns = -info.w;
    info.uv = isect.uv;
    info.primitive = this;
    info.epsilon = 2.5f*isect.w;
}

void Curves::prepareForRender()
{
    std::vector<BvhBuilder<2>::Primitive> prims;
    prims.reserve(_nodeCount - 2*_curveCount);

    float widthScale = _transform.extractScaleVec().avg();

    for (Vec4f &data : _nodeData) {
        Vec3f newP = _transform*data.xyz();
        data.x() = newP.x();
        data.y() = newP.y();
        data.z() = newP.z();
        data.w() *= widthScale;
    }

    for (uint32 i = 0; i < _curveCount; ++i) {
        uint32 start = 0;
        if (i > 0)
            start = _curveEnds[i - 1];

        for (uint32 t = start + 2; t < _curveEnds[i]; ++t) {
            const Vec4f &p0 = _nodeData[t - 2];
            const Vec4f &p1 = _nodeData[t - 1];
            const Vec4f &p2 = _nodeData[t - 0];

            prims.emplace_back(
                curveBox(p0, p1, p2),
                (p0.xyz() + p1.xyz() + p2.xyz())*(1.0f/3.0f),
                t
            );
        }
    }

    _bvh.reset(new BinaryBvh(std::move(prims), 2));

    //_needsRayTransform = true;

    computeBounds();
}

void Curves::cleanupAfterRender()
{
    _bvh.reset();
    loadCurves();
}

}
#include "TrenchCapGeometry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace trench {
namespace {
constexpr double slopeLimit = 0.36397023426620234; // tan(20 degrees)
constexpr double feasibleTolerance = 2e-8;
constexpr double mergeTolerance = 2e-9;
constexpr double rankTolerance = 1e-12;
constexpr double objectiveTolerance = 1e-10;
struct V {
    double x = 0, y = 0, z = 0;
    double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
V operator+(V a, V b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
V operator-(V a, V b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
V operator*(V a, double b) { return {a.x*b, a.y*b, a.z*b}; }
double Dot(V a, V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
V Cross(V a, V b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
double Norm(V a) { return std::sqrt(Dot(a,a)); }
bool Finite(double x) { return std::isfinite(x); }
bool Finite(TrenchVector p) { return Finite(p.x)&&Finite(p.y)&&Finite(p.z); }
bool Finite(const TrenchTransform& t) {
    return Finite(t.xAxis)&&Finite(t.yAxis)&&Finite(t.zAxis)&&Finite(t.origin);
}
struct Plane { V n; double d; }; // n.x*a + n.y*b + n.z*(h-Hseam) <= d
struct Row { V n; double target; };
using Polygon = std::vector<V>;
using Polytope = std::vector<Polygon>;
void AppendUnique(Polygon& points, V p) {
    for (const auto q : points) if (Norm(p-q) <= mergeTolerance) return;
    points.push_back(p);
}
Plane Normalized(V n, double d) {
    const double length = Norm(n);
    return {n*(1/length), d/length};
}
void Bound(std::vector<Plane>& planes, V n, double lower, double upper) {
    planes.push_back(Normalized(n,upper));
    planes.push_back(Normalized(n*(-1),-lower));
}
Polytope Box() {
    const double s = slopeLimit, h = 64;
    return {{{-s,-s,-h},{-s,-s,h},{-s,s,h},{-s,s,-h}},
            {{s,-s,-h},{s,s,-h},{s,s,h},{s,-s,h}},
            {{-s,-s,-h},{s,-s,-h},{s,-s,h},{-s,-s,h}},
            {{-s,s,-h},{-s,s,h},{s,s,h},{s,s,-h}},
            {{-s,-s,-h},{-s,s,-h},{s,s,-h},{s,-s,-h}},
            {{-s,-s,h},{s,-s,h},{s,s,h},{-s,s,h}}};
}
// Clip a convex 3D polytope, retaining lower-dimensional faces as well. Unlike
// enumerating all constraint triples this grows with the actual hull complexity.
bool Clip(Polytope& body, const Plane& plane) {
    double maximum=-(std::numeric_limits<double>::infinity)();
    double minimum=(std::numeric_limits<double>::infinity)();
    for(const auto& face:body)for(const auto p:face) {
        const double distance=Dot(plane.n,p)-plane.d;
        maximum=(std::max)(maximum,distance);minimum=(std::min)(minimum,distance);
    }
    // A redundant/touching plane must not duplicate a coplanar face on every
    // sample. In particular repeated contacts may share exactly one boundary.
    if(maximum<=mergeTolerance)return !body.empty();
    if(minimum>mergeTolerance){body.clear();return false;}
    Polytope next;
    Polygon cut;
    for (const auto& face : body) {
        Polygon clipped;
        if (face.empty()) continue;
        V before = face.back();
        double db = Dot(plane.n,before)-plane.d;
        for (const auto after : face) {
            const double da = Dot(plane.n,after)-plane.d;
            const bool ib = db <= 0, ia = da <= 0;
            if (ib != ia) {
                const V p = before+(after-before)*(db/(db-da));
                AppendUnique(clipped,p); AppendUnique(cut,p);
            }
            if (ia) AppendUnique(clipped,after);
            if (std::fabs(da) <= mergeTolerance) AppendUnique(cut,after);
            before = after; db = da;
        }
        if (!clipped.empty()) next.push_back(std::move(clipped));
    }
    if (next.empty()) { body.clear(); return false; }
    if (!cut.empty()) {
        V center{};
        for (const auto p : cut) center = center+p;
        center = center*(1/static_cast<double>(cut.size()));
        const V reference = std::fabs(plane.n.x)<0.8 ? V{1,0,0}:V{0,1,0};
        V u=Cross(plane.n,reference); u=u*(1/Norm(u));
        const V v=Cross(plane.n,u);
        std::sort(cut.begin(),cut.end(),[&](V a,V b) {
            const double aa=std::atan2(Dot(a-center,v),Dot(a-center,u));
            const double ab=std::atan2(Dot(b-center,v),Dot(b-center,u));
            if (aa != ab) return aa < ab;
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.z < b.z;
        });
        next.push_back(std::move(cut));
    }
    body=std::move(next);
    return true;
}
bool Feasible(V x,const std::vector<Plane>& planes) {
    if (!Finite(x.x)||!Finite(x.y)||!Finite(x.z)) return false;
    for (const auto& p:planes) if(Dot(p.n,x)-p.d>feasibleTolerance) return false;
    return true;
}

// Jacobi diagonalization of an at-most-3x3 symmetric positive semidefinite
// normal matrix. Eigenvectors are columns; small eigenvalues identify the exact
// least-squares minimizer's remaining affine freedom.
void Eigen(double (&a)[3][3],int n,double (&vectors)[3][3]) {
    for(int i=0;i<n;++i) vectors[i][i]=1;
    for(int sweep=0;sweep<40;++sweep) {
        int p=0,q=0; double off=0,scale=0;
        for(int i=0;i<n;++i) {
            scale=(std::max)(scale,std::fabs(a[i][i]));
            for(int j=i+1;j<n;++j) if(std::fabs(a[i][j])>off){p=i;q=j;off=std::fabs(a[i][j]);}
        }
        if(off<=1e-15*(std::max)(1.0,scale)) break;
        const double angle=0.5*std::atan2(2*a[p][q],a[q][q]-a[p][p]);
        const double c=std::cos(angle),s=std::sin(angle);
        const double app=a[p][p],aqq=a[q][q],apq=a[p][q];
        for(int i=0;i<n;++i) if(i!=p&&i!=q) {
            const double aip=a[i][p],aiq=a[i][q];
            a[i][p]=a[p][i]=c*aip-s*aiq;
            a[i][q]=a[q][i]=s*aip+c*aiq;
        }
        a[p][p]=c*c*app-2*c*s*apq+s*s*aqq;
        a[q][q]=s*s*app+2*c*s*apq+c*c*aqq;
        a[p][q]=a[q][p]=0;
        for(int i=0;i<n;++i) {
            const double vip=vectors[i][p],viq=vectors[i][q];
            vectors[i][p]=c*vip-s*viq;vectors[i][q]=s*vip+c*viq;
        }
    }
}
void Minimize(const std::vector<Row>& rows,V& point,std::vector<V>& basis) {
    const int n=static_cast<int>(basis.size());
    if(n==0)return;
    double matrix[3][3]{},rhs[3]{},eigenvectors[3][3]{};
    for(const auto& row:rows) {
        double a[3]{};
        const double residual=row.target-Dot(row.n,point);
        for(int i=0;i<n;++i)a[i]=Dot(row.n,basis[static_cast<std::size_t>(i)]);
        for(int i=0;i<n;++i){rhs[i]+=a[i]*residual;for(int j=0;j<n;++j)matrix[i][j]+=a[i]*a[j];}
    }
    Eigen(matrix,n,eigenvectors);
    double scale=0;for(int i=0;i<n;++i)scale=(std::max)(scale,std::fabs(matrix[i][i]));
    const double threshold=rankTolerance*(std::max)(1.0,scale);
    V delta{};std::vector<V> nullspace;
    for(int j=0;j<n;++j) {
        V direction{};double projected=0;
        for(int i=0;i<n;++i){direction=direction+basis[static_cast<std::size_t>(i)]*eigenvectors[i][j];projected+=eigenvectors[i][j]*rhs[i];}
        if(matrix[j][j]>threshold)delta=delta+direction*(projected/matrix[j][j]);
        else nullspace.push_back(direction);
    }
    point=point+delta;basis=std::move(nullspace);
}
double Cost(const std::vector<Row>& rows,V x) {
    double result=0;for(const auto& row:rows){const double e=Dot(row.n,x)-row.target;result+=e*e;}return result;
}
struct Best {bool exists=false;V point{};std::array<double,3> cost{};};
void Candidate(V point,std::vector<V> basis,const std::vector<Plane>& constraints,
               const std::vector<Row>& primary,const std::vector<Row>& secondary,Best& best) {
    Minimize(primary,point,basis);Minimize(secondary,point,basis);
    static const std::vector<Row> norm{{{1,0,0},0},{{0,1,0},0},{{0,0,1},0}};
    Minimize(norm,point,basis);
    if(!Feasible(point,constraints))return;
    const std::array<double,3> costs{Cost(primary,point),Cost(secondary,point),Dot(point,point)};
    bool better=!best.exists;
    if(best.exists)for(std::size_t i=0;i<costs.size();++i){
        const double tolerance=objectiveTolerance*(std::max)({1.0,std::fabs(costs[i]),std::fabs(best.cost[i])});
        if(costs[i]<best.cost[i]-tolerance){better=true;break;}
        if(costs[i]>best.cost[i]+tolerance)break;
        if(i+1==costs.size()) {
            if(point.x!=best.point.x)better=point.x<best.point.x;
            else if(point.y!=best.point.y)better=point.y<best.point.y;
            else better=point.z<best.point.z;
        }
    }
    if(better)best={true,point,costs};
}
bool Optimize(const Polytope& body,const std::vector<Plane>& constraints,
              const std::vector<Row>& primary,const std::vector<Row>& secondary,Best& best) {
    Candidate({},{{1,0,0},{0,1,0},{0,0,1}},constraints,primary,secondary,best);
    // A feasible unconstrained lexicographic minimum is also the global minimum
    // on the restricted polytope. Boundary enumeration cannot improve it.
    if (best.exists) return true;
    for(const auto& face:body) {
        if(face.empty())continue;
        V center{};for(const auto p:face)center=center+p;
        center=center*(1/static_cast<double>(face.size()));
        std::vector<V> basis;
        for(const auto p:face) {
            V direction=p-center;
            for(const auto b:basis)direction=direction-b*Dot(direction,b);
            const double length=Norm(direction);
            if(length>mergeTolerance)basis.push_back(direction*(1/length));
            if(basis.size()==2)break;
        }
        Candidate(center,basis,constraints,primary,secondary,best);
        for(std::size_t i=0;i<face.size();++i) {
            const V p=face[i],next=face[(i+1)%face.size()],direction=next-p;
            Candidate(p,{},constraints,primary,secondary,best);
            const double length=Norm(direction);
            if(length>mergeTolerance)Candidate(p,{direction*(1/length)},constraints,primary,secondary,best);
        }
    }
    return best.exists;
}
double WorldZ(const TrenchTransform& t,TrenchVector p) {
    return static_cast<double>(t.origin.z)+static_cast<double>(t.xAxis.z)*p.x+
        static_cast<double>(t.yAxis.z)*p.y+static_cast<double>(t.zAxis.z)*p.z;
}
} // namespace

TrenchVector TransformCapPoint(const TrenchTransform& t,TrenchVector p) noexcept {
    return {t.origin.x+t.xAxis.x*p.x+t.yAxis.x*p.y+t.zAxis.x*p.z,
            t.origin.y+t.xAxis.y*p.x+t.yAxis.y*p.y+t.zAxis.y*p.z,
            t.origin.z+t.xAxis.z*p.x+t.yAxis.z*p.y+t.zAxis.z*p.z};
}
bool SameCapCutFootprint(const TrenchTransform& current, const TrenchTransform& proposed) noexcept {
    if (!Finite(current) || !Finite(proposed) ||
        std::hypot(static_cast<double>(current.origin.x) - proposed.origin.x,
                   static_cast<double>(current.origin.y) - proposed.origin.y) > 0.05) return false;
    for (const auto axis : {0, 1}) {
        const auto& a = axis == 0 ? current.xAxis : current.yAxis;
        const auto& b = axis == 0 ? proposed.xAxis : proposed.yAxis;
        if (std::fabs(a.x - b.x) > 0.0001f || std::fabs(a.y - b.y) > 0.0001f) return false;
    }
    return true;
}
const char* CapGeometryStatusName(CapGeometryStatus status) noexcept {
    switch(status){
    case CapGeometryStatus::Solved:return "solved";
    case CapGeometryStatus::ContactMismatch:return "contact_mismatch";
    case CapGeometryStatus::RimInfeasible:return "rim_infeasible";
    case CapGeometryStatus::OutOfDomain:return "out_of_domain";
    default:return "invalid_sample";
    }
}

CapGeometryResult SolveTrenchCapGeometry(const CapGeometryInput& in) {
    CapGeometryResult result;
    if(!in.profile||in.profile->contact.size()<4||in.profile->contact.size()>32||
       in.profile->rimSamples.size()<3||in.profile->rimSamples.size()>128||
       in.contactWorld.size()!=in.profile->contact.size()||in.groundHeights.size()!=in.profile->rimSamples.size()||
       !Finite(in.origin.x)||!Finite(in.origin.y)||!Finite(in.inward.x)||!Finite(in.inward.y)||
       (in.previous&&!Finite(*in.previous)))return result;
    const double length=std::hypot(in.inward.x,in.inward.y);
    if(length<1e-8)return result;
    const double ux=in.inward.x/length,uy=in.inward.y/length;
    double seam=0;
    for(std::size_t i=0;i<in.profile->contact.size();++i) {
        const auto q=in.profile->contact[i].capBind,p=in.contactWorld[i];
        if(!Finite(q)||!Finite(p)){result.firstInvalidPoint=i;return result;}
        const double xy=std::hypot(in.origin.x+ux*q.x-uy*q.y-p.x,in.origin.y+uy*q.x+ux*q.y-p.y);
        result.maxContactXY=(std::max)(result.maxContactXY,xy);
        if(xy>0.25){result.status=CapGeometryStatus::ContactMismatch;result.firstInvalidPoint=i;return result;}
        seam+=(static_cast<double>(p.z)-q.z)/static_cast<double>(in.contactWorld.size());
    }
    if(!Finite(seam))return result;
    std::vector<Plane> constraints;
    Bound(constraints,{1,0,0},-slopeLimit,slopeLimit);
    Bound(constraints,{0,1,0},-slopeLimit,slopeLimit);
    Bound(constraints,{0,0,1},-64,64);
    std::vector<Row> primary,secondary;
    for(std::size_t i=0;i<in.profile->contact.size();++i) {
        const auto q=in.profile->contact[i].capBind;
        const double target=static_cast<double>(in.contactWorld[i].z)-q.z-seam;
        Bound(constraints,{q.x,q.y,1},target-0.5,target+0.5);
        const double previous=in.previous?WorldZ(*in.previous,q)-q.z-seam:0;
        secondary.push_back({{q.x,q.y,1},previous});
    }
    const std::size_t contactConstraintCount=constraints.size();
    for(std::size_t i=0;i<in.profile->rimSamples.size();++i) {
        const auto& sample=in.profile->rimSamples[i];const auto l=sample.lip,s=sample.skirt;
        const double ground=in.groundHeights[i];
        if(!Finite(l)||!Finite(s)||!Finite(ground)||l.x!=s.x||l.y!=s.y||s.z>=l.z) {
            result.firstInvalidPoint=i;return result;
        }
        const V n{l.x,l.y,1};
        Bound(constraints,n,ground-4-l.z-seam,ground+0.5-l.z-seam);
        Bound(constraints,n,ground-8-s.z-seam,ground-0.5-s.z-seam);
        primary.push_back({n,ground-0.25-l.z-seam});
        for(const auto q:{l,s})secondary.push_back({{q.x,q.y,1},in.previous?WorldZ(*in.previous,q)-q.z-seam:0});
    }
    Best best;
    // Common planar surfaces have a feasible unconstrained solution. Verify all
    // hard bounds first and avoid constructing a hull that cannot improve it.
    Candidate({},{{1,0,0},{0,1,0},{0,0,1}},constraints,primary,secondary,best);
    if (!best.exists) {
        Polytope body=Box();
        for(std::size_t i=6;i<contactConstraintCount;++i)if(!Clip(body,constraints[i])) {
            result.status=CapGeometryStatus::OutOfDomain;result.firstInvalidPoint=(i-6)/2;return result;
        }
        for(std::size_t i=contactConstraintCount;i<constraints.size();++i)if(!Clip(body,constraints[i])) {
            result.status=CapGeometryStatus::RimInfeasible;result.firstInvalidPoint=(i-contactConstraintCount)/4;return result;
        }
        if(!Optimize(body,constraints,primary,secondary,best)){result.status=CapGeometryStatus::OutOfDomain;return result;}
    }
    result.a=best.point.x;result.b=best.point.y;result.h=seam+best.point.z;
    result.transform={{static_cast<float>(ux),static_cast<float>(uy),static_cast<float>(result.a)},
                      {static_cast<float>(-uy),static_cast<float>(ux),static_cast<float>(result.b)},
                      {0,0,1},{in.origin.x,in.origin.y,static_cast<float>(result.h)}};
    const V rounded{result.transform.xAxis.z,result.transform.yAxis.z,static_cast<double>(result.transform.origin.z)-seam};
    // Float rounding is verified in world-height units as well, rather than
    // allowing a normalized-plane epsilon to grow with a large authored point.
    for(std::size_t i=0;i<in.profile->contact.size();++i) {
        const auto q=in.profile->contact[i].capBind;
        const double error=std::fabs(WorldZ(result.transform,q)-in.contactWorld[i].z);
        result.maxContactZ=(std::max)(result.maxContactZ,error);
        if(error>0.50005){result.status=CapGeometryStatus::OutOfDomain;result.firstInvalidPoint=i;return result;}
    }
    result.minLipClearance=result.minSkirtClearance=(std::numeric_limits<double>::infinity)();
    result.maxLipClearance=result.maxSkirtClearance=-(std::numeric_limits<double>::infinity)();
    for(std::size_t i=0;i<in.profile->rimSamples.size();++i) {
        const auto& sample=in.profile->rimSamples[i];const double ground=in.groundHeights[i];
        const double lip=WorldZ(result.transform,sample.lip)-ground,skirt=WorldZ(result.transform,sample.skirt)-ground;
        result.minLipClearance=(std::min)(result.minLipClearance,lip);result.maxLipClearance=(std::max)(result.maxLipClearance,lip);
        result.minSkirtClearance=(std::min)(result.minSkirtClearance,skirt);result.maxSkirtClearance=(std::max)(result.maxSkirtClearance,skirt);
        if(lip < -4.00005||lip>0.50005||skirt < -8.00005||skirt > -0.49995) {
            result.status=CapGeometryStatus::OutOfDomain;result.firstInvalidPoint=i;return result;
        }
    }
    bool roundedFeasible=Finite(result.transform);
    for(const auto& p:constraints)if(Dot(p.n,rounded)-p.d>5e-5)roundedFeasible=false;
    if(!roundedFeasible){result.status=CapGeometryStatus::OutOfDomain;return result;}
    result.rimObjective=best.cost[0];result.previousObjective=best.cost[1];
    result.status=CapGeometryStatus::Solved;
    return result;
}
} // namespace trench

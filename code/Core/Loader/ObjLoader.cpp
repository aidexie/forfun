
#include "ObjLoader.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <limits>
#include <cmath>
#include <DirectXMath.h>

struct VIdx { int v=-1, vt=-1, vn=-1; };
static inline VIdx parseVIdx(const std::string& s) {
    VIdx idx{}; std::string buf; int slot=0; int vals[3]={-1,-1,-1};
    for (char c: s){ if (c=='/'){ vals[slot++]=buf.empty()? -1:std::stoi(buf); buf.clear(); } else buf.push_back(c); }
    vals[slot++]=buf.empty()? -1:std::stoi(buf);
    idx.v=vals[0]; idx.vt=vals[1]; idx.vn=vals[2]; return idx;
}
static inline void trim(std::string& s){ size_t a=0; while (a<s.size() && isspace((unsigned char)s[a])) ++a; size_t b=s.size(); while (b>a && isspace((unsigned char)s[b-1])) --b; s=s.substr(a,b-a); }
static inline int fix(int idx, int n){ return idx>0? idx-1 : (idx<0? n+idx : -1); }

bool LoadOBJ_PNT(const std::string& path, SMeshCPU_PNT& out, bool flipZ, bool flipWinding)
{
    std::ifstream in(path); if (!in) return false;
    std::vector<float> P; std::vector<float> N; std::vector<float> T;

    struct Key{ int v,vt,vn; bool operator==(const Key&o)const{return v==o.v&&vt==o.vt&&vn==o.vn;} };
    struct KH{ size_t operator()(const Key&k)const{ return (std::hash<int>()(k.v)*73856093)^(std::hash<int>()(k.vt)*19349663)^(std::hash<int>()(k.vn)*83492791); } };
    std::unordered_map<Key, uint32_t, KH> map;

    out.vertices.clear(); out.indices.clear();
    std::string line;
    while (std::getline(in,line)){
        trim(line); if (line.empty()||line[0]=='#') continue;
        std::istringstream iss(line); std::string tag; iss>>tag;
        if (tag=="v"){ float x,y,z; iss>>x>>y>>z; if (flipZ) z=-z; P.insert(P.end(),{x,y,z}); }
        else if (tag=="vn"){ float x,y,z; iss>>x>>y>>z; if (flipZ) z=-z; N.insert(N.end(),{x,y,z}); }
        else if (tag=="vt"){ float u,v; iss>>u>>v; T.insert(T.end(),{u,v}); }
        else if (tag=="f"){
            std::vector<VIdx> face; std::string s; while (iss>>s) face.push_back(parseVIdx(s));
            if (face.size()<3) continue;
            for (size_t k=1;k+1<face.size();++k){
                VIdx tri[3] = { face[0], face[k], face[k+1] };
                for (int t=0;t<3;++t){
                    int nv=(int)P.size()/3, nn=(int)N.size()/3, nt=(int)T.size()/2;
                    int iv=fix(tri[t].v,nv), it=fix(tri[t].vt,nt), in=fix(tri[t].vn,nn);
                    Key key{iv,it,in}; auto itK=map.find(key); uint32_t oi;
                    if (itK==map.end()){
                        SVertexPNT v{};
                        v.px=P[iv*3+0]; v.py=P[iv*3+1]; v.pz=P[iv*3+2];
                        if (in>=0){ v.nx=N[in*3+0]; v.ny=N[in*3+1]; v.nz=N[in*3+2]; }
                        else { v.nx=0; v.ny=1; v.nz=0; }
                        if (it>=0){ v.u=T[it*2+0]; v.v=T[it*2+1]; } else { v.u=0; v.v=0; }
                        v.tx=v.ty=v.tz=0; v.tw=1;
                        v.r=v.g=v.b=v.a=1.0f;  // OBJ doesn't support vertex colors, default to white
                        oi=(uint32_t)out.vertices.size(); out.vertices.push_back(v); map.emplace(key,oi);
                    } else oi=itK->second;
                    out.indices.push_back(oi);
                }
                if (flipWinding) std::swap(out.indices[out.indices.size()-3], out.indices[out.indices.size()-2]);
            }
        }
    }
    // generate normals if missing
    if (N.empty()){
        std::vector<DirectX::XMFLOAT3> acc(out.vertices.size(),{0,0,0});
        for (size_t i=0;i+2<out.indices.size();i+=3){
            auto i0=out.indices[i], i1=out.indices[i+1], i2=out.indices[i+2];
            using namespace DirectX;
            XMVECTOR p0=XMLoadFloat3((XMFLOAT3*)&out.vertices[i0].px);
            XMVECTOR p1=XMLoadFloat3((XMFLOAT3*)&out.vertices[i1].px);
            XMVECTOR p2=XMLoadFloat3((XMFLOAT3*)&out.vertices[i2].px);
            XMVECTOR n=XMVector3Normalize(XMVector3Cross(p1-p0,p2-p0));
            DirectX::XMFLOAT3 nn; XMStoreFloat3(&nn,n);
            auto add=[&](uint32_t ii){ acc[ii].x+=nn.x; acc[ii].y+=nn.y; acc[ii].z+=nn.z; };
            add(i0); add(i1); add(i2);
        }
        for (size_t i=0;i<out.vertices.size();++i){
            using namespace DirectX;
            XMVECTOR n=XMVector3Normalize(XMLoadFloat3(&acc[i]));
            DirectX::XMFLOAT3 nn; XMStoreFloat3(&nn,n);
            out.vertices[i].nx=nn.x; out.vertices[i].ny=nn.y; out.vertices[i].nz=nn.z;
        }
    }
    ComputeTangents(out.vertices, out.indices);
    return !out.vertices.empty() && !out.indices.empty();
}

static void ComputeBBox(const SMeshCPU_PNT& m, float mn[3], float mx[3]){
    mn[0]=mn[1]=mn[2]= std::numeric_limits<float>::infinity();
    mx[0]=mx[1]=mx[2]=-std::numeric_limits<float>::infinity();
    for (auto& v: m.vertices){
        mn[0]=std::min(mn[0],v.px); mn[1]=std::min(mn[1],v.py); mn[2]=std::min(mn[2],v.pz);
        mx[0]=std::max(mx[0],v.px); mx[1]=std::max(mx[1],v.py); mx[2]=std::max(mx[2],v.pz);
    }
}
void RecenterAndScale(SMeshCPU_PNT& m, float targetDiag){
    float mn[3],mx[3]; ComputeBBox(m,mn,mx);
    float cx=0.5f*(mn[0]+mx[0]), cy=0.5f*(mn[1]+mx[1]), cz=0.5f*(mn[2]+mx[2]);
    float dx=mx[0]-mn[0], dy=mx[1]-mn[1], dz=mx[2]-mn[2];
    float diag = std::max(0.0001f, std::sqrt(dx*dx+dy*dy+dz*dz));
    float s = targetDiag/diag;
    for (auto& v: m.vertices){ v.px=(v.px-cx)*s; v.py=(v.py-cy)*s; v.pz=(v.pz-cz)*s; }
}

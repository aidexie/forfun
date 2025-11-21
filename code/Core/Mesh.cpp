
#include "Mesh.h"
#include <DirectXMath.h>
using namespace DirectX;

void ComputeTangents(std::vector<SVertexPNT>& vtx, const std::vector<uint32_t>& idx)
{
    std::vector<XMFLOAT3> tan(vtx.size(), {0,0,0});
    std::vector<XMFLOAT3> bit(vtx.size(), {0,0,0});

    for (size_t i=0;i+2<idx.size();i+=3){
        uint32_t i0=idx[i], i1=idx[i+1], i2=idx[i+2];
        auto &v0=vtx[i0], &v1=vtx[i1], &v2=vtx[i2];

        XMVECTOR p0 = XMVectorSet(v0.px,v0.py,v0.pz,1);
        XMVECTOR p1 = XMVectorSet(v1.px,v1.py,v1.pz,1);
        XMVECTOR p2 = XMVectorSet(v2.px,v2.py,v2.pz,1);

        float du1=v1.u - v0.u, dv1=v1.v - v0.v;
        float du2=v2.u - v0.u, dv2=v2.v - v0.v;

        XMVECTOR e1 = XMVectorSubtract(p1,p0);
        XMVECTOR e2 = XMVectorSubtract(p2,p0);

        float r = (du1*dv2 - du2*dv1);
        if (fabsf(r) < 1e-8f) r = (r>=0? 1e-8f : -1e-8f);
        r = 1.0f / r;

        XMVECTOR t = XMVectorScale( XMVectorSubtract( XMVectorScale(e1,dv2), XMVectorScale(e2,dv1) ), r);
        XMVECTOR b = XMVectorScale( XMVectorSubtract( XMVectorScale(e2,du1), XMVectorScale(e1,du2) ), r);

        auto acc = [&](uint32_t ii, XMVECTOR T, XMVECTOR B){
            XMFLOAT3 tt,bb; XMStoreFloat3(&tt,T); XMStoreFloat3(&bb,B);
            tan[ii].x += tt.x; tan[ii].y += tt.y; tan[ii].z += tt.z;
            bit[ii].x += bb.x; bit[ii].y += bb.y; bit[ii].z += bb.z;
        };
        acc(i0,t,b); acc(i1,t,b); acc(i2,t,b);
    }

    for (size_t i=0;i<vtx.size();++i){
        XMVECTOR n = XMVector3Normalize(XMVectorSet(vtx[i].nx,vtx[i].ny,vtx[i].nz,0));
        XMVECTOR t = XMVector3Normalize(XMVectorSet(tan[i].x,tan[i].y,tan[i].z,0));
        // Gram-Schmidt
        t = XMVector3Normalize( XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n,t))) ) );
        XMVECTOR bCalc = XMVector3Cross(n,t);
        XMVECTOR bIn   = XMVector3Normalize(XMVectorSet(bit[i].x,bit[i].y,bit[i].z,0));
        float sign = XMVectorGetX( XMVector3Dot(bCalc, bIn) ) < 0 ? -1.0f : +1.0f;

        DirectX::XMFLOAT3 tt; XMStoreFloat3(&tt, t);
        vtx[i].tx=tt.x; vtx[i].ty=tt.y; vtx[i].tz=tt.z; vtx[i].tw=sign;
    }
}

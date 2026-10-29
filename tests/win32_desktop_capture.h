#include <d3d11.h>
#include <dxgi1_2.h>
typedef struct {ID3D11Device *dev;ID3D11DeviceContext *ctx;IDXGIOutputDuplication *dup;ID3D11Texture2D *staging;RECT bounds;} DXCapture;
static int dx_init(DXCapture *d,RECT target) {
    IDXGIFactory1 *factory=NULL;HRESULT hr=CreateDXGIFactory1(&IID_IDXGIFactory1,(void**)&factory);
    if(FAILED(hr)){printf("factory %lx\n",hr);return 0;}
    for(UINT a=0;;a++) {
        IDXGIAdapter1 *adapter=NULL;if(FAILED(IDXGIFactory1_EnumAdapters1(factory,a,&adapter)))break;
        for(UINT o=0;;o++) {
            IDXGIOutput *output=NULL;if(FAILED(IDXGIAdapter1_EnumOutputs(adapter,o,&output)))break;
            DXGI_OUTPUT_DESC desc={0};
            if(FAILED(IDXGIOutput_GetDesc(output,&desc))){IDXGIOutput_Release(output);continue;}
            if(desc.AttachedToDesktop && desc.Rotation==DXGI_MODE_ROTATION_IDENTITY && target.left>=desc.DesktopCoordinates.left && target.top>=desc.DesktopCoordinates.top && target.right<=desc.DesktopCoordinates.right && target.bottom<=desc.DesktopCoordinates.bottom) {
                IDXGIOutput1 *out1=NULL;
                hr=D3D11CreateDevice((IDXGIAdapter*)adapter,D3D_DRIVER_TYPE_UNKNOWN,NULL,0,NULL,0,D3D11_SDK_VERSION,&d->dev,NULL,&d->ctx);
                if(SUCCEEDED(hr))hr=IDXGIOutput_QueryInterface(output,&IID_IDXGIOutput1,(void**)&out1);
                if(SUCCEEDED(hr))hr=IDXGIOutput1_DuplicateOutput(out1,(IUnknown*)d->dev,&d->dup);
                if(out1)IDXGIOutput1_Release(out1);
                IDXGIOutput_Release(output);IDXGIAdapter1_Release(adapter);IDXGIFactory1_Release(factory);
                d->bounds=desc.DesktopCoordinates;
                printf("DXGI init=%lx output=%ld,%ld,%ld,%ld\n",hr,d->bounds.left,d->bounds.top,d->bounds.right,d->bounds.bottom);
                return SUCCEEDED(hr);
            }
            IDXGIOutput_Release(output);
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);return 0;
}
static int dx_frame(DXCapture *d,RECT target,BYTE *pixels,LARGE_INTEGER *present) {
    IDXGIResource *res=NULL;DXGI_OUTDUPL_FRAME_INFO frame;
    HRESULT hr=IDXGIOutputDuplication_AcquireNextFrame(d->dup,16,&frame,&res);
    if(hr==DXGI_ERROR_WAIT_TIMEOUT)return 0;
    if(FAILED(hr)){printf("DXGI acquire=%lx\n",hr);return -1;}
    *present=frame.LastPresentTime;
    ID3D11Texture2D *image=NULL;
    hr=IDXGIResource_QueryInterface(res,&IID_ID3D11Texture2D,(void**)&image);
    IDXGIResource_Release(res);
    int width=target.right-target.left,height=target.bottom-target.top;
    if(SUCCEEDED(hr) && !d->staging) {
        D3D11_TEXTURE2D_DESC desc={0};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        hr=ID3D11Device_CreateTexture2D(d->dev,&desc,NULL,&d->staging);
    }
    if(SUCCEEDED(hr)) {
        D3D11_BOX box={(UINT)(target.left-d->bounds.left),(UINT)(target.top-d->bounds.top),0,(UINT)(target.right-d->bounds.left),(UINT)(target.bottom-d->bounds.top),1};
        ID3D11DeviceContext_CopySubresourceRegion(d->ctx,(ID3D11Resource*)d->staging,0,0,0,0,(ID3D11Resource*)image,0,&box);
        D3D11_MAPPED_SUBRESOURCE map;
        hr=ID3D11DeviceContext_Map(d->ctx,(ID3D11Resource*)d->staging,0,D3D11_MAP_READ,0,&map);
        if(SUCCEEDED(hr)) {
            for(int y=0;y<height;y++)memcpy(pixels+(size_t)y*width*4,(BYTE*)map.pData+(size_t)y*map.RowPitch,(size_t)width*4);
            ID3D11DeviceContext_Unmap(d->ctx,(ID3D11Resource*)d->staging,0);
        }
    }
    if(image)ID3D11Texture2D_Release(image);
    IDXGIOutputDuplication_ReleaseFrame(d->dup);
    return SUCCEEDED(hr)?1:-1;
}
static void dx_free(DXCapture *d) {
    if(d->staging)ID3D11Texture2D_Release(d->staging);
    if(d->dup)IDXGIOutputDuplication_Release(d->dup);
    if(d->ctx)ID3D11DeviceContext_Release(d->ctx);
    if(d->dev)ID3D11Device_Release(d->dev);
}
/* Test-only DXGI readback of the desktop after composition. Reject unavailable
   devices/rotated outputs; never substitute GDI intermediate-window captures. */

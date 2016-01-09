/********************************************************************************
 Copyright (C) 2012 Hugh Bailey <obs.jim@gmail.com>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
********************************************************************************/


#include "GraphicsCaptureHook.h"

#include <D3D10_1.h>
#include <D3D11.h>
#include "DXGIStuff.h"

#include <sys/stat.h>
#include <set>


FARPROC                 oldD3D11Release = NULL;
FARPROC                 newD3D11Release = NULL;

CaptureInfo             d3d11CaptureInfo;

extern LPVOID           lpCurrentSwap;
extern LPVOID           lpCurrentDevice;
SharedTexData           *texData;
SharedTexData           *texDataDepth;
extern DWORD            curCapture;
extern BOOL             bHasTextures;
extern BOOL             bIsMultisampled;
extern LONGLONG         lastTime;

extern DXGI_FORMAT      dxgiFormat;
ID3D11Resource          *copyTextureGame = NULL;
ID3D11Resource          *copyTextureGame2 = NULL;
HANDLE                  sharedHandle = NULL;
HANDLE                  sharedHandle2 = NULL;
HANDLE                  sharedDepthHandle = NULL;

extern bool bD3D101Hooked;

ID3D11RenderTargetView *cq_backbuffer;    // cq the pointer to our back buffer
ID3D11DepthStencilView *cq_zbuffer;       // cq the pointer to our depth buffer

ID3D11Resource  *cq_pDepthBuffer;         // cq trying to get at depth buffer with CPU / CUDA
ID3D11Texture2D *cq_pDepthBufferCopy;     // cq trying to get at depth buffer with CPU / CUDA
set<ID3D11Resource*> cq_depthResources;
void copySensorResources(IDXGISwapChain *swap, bool shouldCopyDepth);
bool setSharedHandleGame2Sensor(ID3D11Device *device);

bool createSharedDepthHandle();

int cq_height;
int cq_width;

bool isFirstDepthHook = true;

float* getDepthsAsFloats(unsigned height, unsigned width, BYTE* depths);

void setFirstDepthHook(bool val)
{
	isFirstDepthHook = val;
}

void saveDepthResource(ID3D11Resource* pResource)
{
	cq_depthResources.insert(pResource);
}

int depthViewTarget = 0;
void setDepthViewTarget(int target)
{
	depthViewTarget = target;
}

inline bool file_exists (const std::string& name) {
  struct stat buffer;   
  return (stat (name.c_str(), &buffer) == 0); 
}

bool shouldWriteDepthTOFile = false;
bool shouldCreateSharedMem = true;
bool cq_shouldWriteFile = false;
std::string depthFileName = "cqDepthFloat.txt";
bool isFirstCopySensors = true;
bool createdSharedMem = false;

bool createSharedMem(ID3D11Resource* cq_savedDepthResource, ID3D11Device* device)
{
	if (cq_savedDepthResource)
	{
		if(createSharedDepthHandle())
		{
			logOutput << CurrentTimeString() << "cqcqcqcqcqcqcqcq created shared depth handle cqcqcqcqcqcqcqcq" << endl;
		}
		else
		{
			logOutput << CurrentTimeString() << "cqcqcqcqcqcqcqcq failed to create shared depth handle cqcqcqcqcqcqcqcq" << endl;
			return false;
		}
	}

	if (setSharedHandleGame2Sensor(device))
	{
		logOutput << CurrentTimeString() << "cqcqcqcqcqcqcqcq created shared camera handle cqcqcqcqcqcqcqcq" << endl;
	}
	else
	{
		logOutput << CurrentTimeString() << "cqcqcqcqcqcqcqcq failed to create shared camera handle cqcqcqcqcqcqcqcq" << endl;
		return false;
	}
		
	logOutput << CurrentTimeString() << "cqcqcqcqcqcqcqcq trying to share mem cqcqcqcqcqcqcqcq" << endl;
	UINT mapId = InitializeSharedMemoryGPUCaptureGame2Sensor(&texDataDepth);
	texDataDepth->depthTexHandle = (DWORD)sharedDepthHandle; // FUCKING HANDLE
	texDataDepth->texHandle = (DWORD)sharedHandle2;
	shouldCreateSharedMem = false;
	logOutput << CurrentTimeString() << "cqcqcqcqcqcqcqcq Created shared mem cqcqcqcqcqcqcqcq" << endl;
	return true;
}

void copySensors(IDXGISwapChain* swap, ID3D11Resource * cq_savedDepthResource, string depthFilename)
{
	if (isFirstCopySensors)
	{
		logOutput << CurrentTimeString() << "cqcqcqcqcqcqcqcq copySensors called first time cqcqcqcqcqcqcqcq" << endl;
		isFirstCopySensors = false;
	}
    ID3D11Device* device = nullptr;
    HRESULT hRes = swap->GetDevice(__uuidof(ID3D11Device), (void**)&device);

    ID3D11DeviceContext *devcon;
    device->GetImmediateContext(&devcon);

	if (cq_savedDepthResource)
	{
		if (isFirstDepthHook)
		{
			// create the depth buffer copy texture
			D3D11_TEXTURE2D_DESC texdCopy;
			ZeroMemory(&texdCopy, sizeof(texdCopy));

			texdCopy.Width = cq_width;
			texdCopy.Height = cq_height;
			texdCopy.ArraySize = 1;
			texdCopy.MipLevels = 1;
			texdCopy.SampleDesc.Count = 1; // Must have BindFlags set if greater than 1.
			texdCopy.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;

			if (shouldWriteDepthTOFile)
			{
				// CPU READ
				texdCopy.BindFlags = NULL; // Must be NULL to read to file with CPU
				texdCopy.Usage = D3D11_USAGE_STAGING; // Must be staging to read from CPU, but means BindFlags cannot be set.
				texdCopy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			}
			else
			{
				// GPU SHARE
				texdCopy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				texdCopy.Usage = D3D11_USAGE_DEFAULT; // Must be staging to read from CPU, but means BindFlags cannot be set.
				texdCopy.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // Cannot be set for CPU reading
			}
			device->CreateTexture2D(&texdCopy, NULL, &cq_pDepthBufferCopy);
			isFirstDepthHook = false;
		}

		devcon->CopyResource(cq_pDepthBufferCopy, cq_savedDepthResource);
	}
	
	if (shouldCreateSharedMem)
	{
		createdSharedMem = createSharedMem(cq_savedDepthResource, device);
	}

	// Copy camera
	ID3D11Resource *backBuffer = nullptr;
	if (createdSharedMem && SUCCEEDED(hRes = swap->GetBuffer(0, IID_ID3D11Resource, (void**)&backBuffer)))
        {
            if(bIsMultisampled)
            {
                devcon->ResolveSubresource(copyTextureGame2, 0, backBuffer, 0, dxgiFormat);
            } 
			
			else
			{
                devcon->CopyResource(copyTextureGame2, backBuffer);	
			}
            backBuffer->Release();
	}

	if (shouldWriteDepthTOFile && cq_savedDepthResource) {
        D3D11_MAPPED_SUBRESOURCE mappedResource;

        HRESULT mapResult = devcon->Map(cq_pDepthBufferCopy, 0, D3D11_MAP_READ, 0, &mappedResource);

        BYTE *pYourBytes = (BYTE *) mappedResource.pData;
        unsigned int uiPitch = mappedResource.RowPitch;

        float *fDepths = getDepthsAsFloats(cq_height, cq_width, pYourBytes);

        std::ofstream myfile;
        myfile.open(depthFilename);
        auto floatWidth = cq_height * cq_width;
        for (auto i = 0; i < floatWidth; i++) {
            myfile << fDepths[i] << " ";
        }
        myfile.close();
        cq_shouldWriteFile = false;
        shouldWriteDepthTOFile = false;
        devcon->Unmap(cq_pDepthBufferCopy, 0);
    }

//    cq_pDepthBufferCopy->Release();
    devcon->Release();
    device->Release();
}

bool isFirstCopySenorResources = true;

void copySensorResources(IDXGISwapChain *swap, bool isCaptureDepthEnabled)
{
	if (isFirstCopySenorResources)
	{
		logOutput << CurrentTimeString() << "cqcqcqcqcqcqcqcq copySensorResources called first time cqcqcqcqcqcqcqcq" << endl;
	}

	auto i = 0;
	string filename = depthFileName + to_string(i);
	if (isCaptureDepthEnabled && cq_depthResources.size() > 0)
	{
		for (auto &depthResource : cq_depthResources)
		{
			if (i == depthViewTarget) // || file_exists(filename) == false )
			{
				copySensors(swap, depthResource, filename);  // TODO: Separate depth more cleanly
			}
			i++;
		}
	}
	else
	{
		copySensors(swap, nullptr, filename); // TODO: Separate depth more cleanly
	}

	isFirstCopySenorResources = false;

}

void clearDepthResources()
{
	cq_depthResources.clear();
}

float* getDepthsAsFloats(unsigned height, unsigned width, BYTE* depths)
{
	static float* fDepths;
	int fDepthsLength = height * width;
	fDepths = new float[fDepthsLength];
	for (int j = 0; j < fDepthsLength; j++)
	{
		// initialize to infinite depth
		fDepths[j] = 0.0;
	}

	for (int y = 0; y < height; y++) 
	{
		for (int x = 0; x < width; x++) 
		{
			float f;
			int fDepthsIndex = y * width + x;
			int i = fDepthsIndex * 8; // 8 bytes per DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS (top 4 are 32 bit floating point depth)
			unsigned char b[] = { depths[i], depths[i + 1], depths[i + 2], depths[i + 3] };
			memcpy(&f, &b, sizeof(f));
			fDepths[fDepthsIndex] = f;
		}
	}
	return fDepths;
}


void ClearD3D11Data()
{
    bHasTextures = false;
    texData = NULL;
//    texDataDepth = NULL;
    sharedHandle = NULL;
//	sharedDepthHandle = NULL;

    SafeRelease(copyTextureGame);
//    SafeRelease(copyTextureGame2);

    DestroySharedMemory();
    keepAliveTime = 0;
    resetCount++;

    logOutput << CurrentTimeString() << "---------------------- Cleared D3D11 Capture ----------------------" << endl;
}

void SetupD3D11(IDXGISwapChain *swapChain)
{
    logOutput << CurrentTimeString() << "setting up d3d11 data" << endl;
    ClearD3D11Data();

    DXGI_SWAP_CHAIN_DESC scd;
    if(SUCCEEDED(swapChain->GetDesc(&scd)))
    {
        d3d11CaptureInfo.format = ConvertGIBackBufferFormat(scd.BufferDesc.Format);

		cq_height = scd.BufferDesc.Height; // cq
		cq_width = scd.BufferDesc.Width;   // cq

//		// create the depth buffer copy texture
//		D3D11_TEXTURE2D_DESC texdCopy;
//		ZeroMemory(&texdCopy, sizeof(texdCopy));
//
//		texdCopy.Width = cq_height;
//		texdCopy.Height = cq_width;
//		texdCopy.ArraySize = 1;
//		texdCopy.MipLevels = 1;
//		texdCopy.SampleDesc.Count = 1; // Must have BindFlags set if greater than 1.
//		texdCopy.Format = DXGI_FORMAT_R32_TYPELESS; 
//		texdCopy.BindFlags = NULL;
//		texdCopy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
//		texdCopy.Usage = D3D11_USAGE_STAGING; // Must be staging to read from CPU, but means BindFlags cannot be set.
//
//		ID3D11Device* dev = NULL;
//		HRESULT hRes = swap->GetDevice(__uuidof(ID3D11Device), (void**)&dev);
//		hRes = dev->CreateTexture2D(&texdCopy, NULL, &cq_pDepthBufferCopy);


        if(d3d11CaptureInfo.format != GS_UNKNOWNFORMAT)
        {
            if( dxgiFormat                   != scd.BufferDesc.Format ||
                d3d11CaptureInfo.cx          != scd.BufferDesc.Width  ||
                d3d11CaptureInfo.cy          != scd.BufferDesc.Height ||
                d3d11CaptureInfo.hwndCapture != (DWORD)scd.OutputWindow)
            {
                dxgiFormat = FixCopyTextureFormat(scd.BufferDesc.Format);
                d3d11CaptureInfo.cx = scd.BufferDesc.Width;
                d3d11CaptureInfo.cy = scd.BufferDesc.Height;
                d3d11CaptureInfo.hwndCapture = (DWORD)scd.OutputWindow;
                bIsMultisampled = scd.SampleDesc.Count > 1;

                logOutput << CurrentTimeString() << "found dxgi format (dx11) of: " << UINT(dxgiFormat) <<
                    ", size: {" << scd.BufferDesc.Width << ", " << scd.BufferDesc.Height <<
                    "}, multisampled: " << (bIsMultisampled ? "true" : "false") << endl;
            }
        }
    }

    lastTime = 0;
    OSInitializeTimer();
}

typedef HRESULT (WINAPI *CREATEDXGIFACTORY1PROC)(REFIID riid, void **ppFactory);


bool createSharedDepthHandle()
{
	logOutput << CurrentTimeString() << "cqcqcqcqcqcqcqcq Trying to create shared depth handle cqcqcqcqcqcqcqcq" << endl;
	// convert texture to d3d11 resource
	if (cq_pDepthBufferCopy->QueryInterface(__uuidof(ID3D11Resource), (void**)&cq_pDepthBufferCopy) != S_OK)
	{
		//			RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Hook: d3d11Tex->QueryInterface(ID3D11Resource) failed, result = " << UINT(hErr) << endl;
		//			cq_pDepthBufferCopy->Release();
		return false;
	}

	// convert d3d11 resource to more general dxgi resource
	IDXGIResource *res2;
	if (cq_pDepthBufferCopy->QueryInterface(__uuidof(IDXGIResource), (void**)&res2) != S_OK)
	{
		//			RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Hook: d3d11DepthTex->QueryInterface(IID_IDXGIResource) failed, result = " << UINT(hErr) << endl;
		//			d3d11DepthTex->Release();
		return false;
	}

	// get shared handle from dxgi resource
	if (res2->GetSharedHandle(&sharedDepthHandle) != S_OK)
	{
		//			RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Hook: res2->GetSharedDepthHandle failed, result = " << UINT(hErr) << endl;
		//			d3d11DepthTex->Release();
		res2->Release();
		return false;
	}

	res2->Release();
	return true;
}

bool setSharedHandleGame2Sensor(ID3D11Device *device)
{
	logOutput << CurrentTimeString() << "cqcqcqcqcqcqcqcq Created shared handle game2sensor cqcqcqcqcqcqcqcq" << endl;
	HRESULT hErr;

    D3D11_TEXTURE2D_DESC texGameDesc;
    ZeroMemory(&texGameDesc, sizeof(texGameDesc));
    texGameDesc.Width               = d3d11CaptureInfo.cx;
    texGameDesc.Height              = d3d11CaptureInfo.cy;
    texGameDesc.MipLevels           = 1;
    texGameDesc.ArraySize           = 1;
    texGameDesc.Format              = dxgiFormat;
    texGameDesc.SampleDesc.Count    = 1;
    texGameDesc.BindFlags           = D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    texGameDesc.Usage               = D3D11_USAGE_DEFAULT;
    texGameDesc.MiscFlags           = D3D11_RESOURCE_MISC_SHARED;

    ID3D11Texture2D *d3d11Tex;
    if(FAILED(hErr = device->CreateTexture2D(&texGameDesc, NULL, &d3d11Tex)))
    {
        RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Hook: creation of intermediary texture Game2Sensor failed, result = " << UINT(hErr) << endl;
        return false;
    }

    if(FAILED(hErr = d3d11Tex->QueryInterface(__uuidof(ID3D11Resource), (void**)&copyTextureGame2)))
    {
        RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Hook: d3d11Tex->QueryInterface(ID3D11Resource) Game2Sensor failed, result = " << UINT(hErr) << endl;
        d3d11Tex->Release();
        return false;
    }

    IDXGIResource *res;
    if(FAILED(hErr = d3d11Tex->QueryInterface(IID_IDXGIResource, (void**)&res)))
    {
        RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Hook: d3d11Tex->QueryInterface(IID_IDXGIResource) Game2Sensor failed, result = " << UINT(hErr) << endl;
        d3d11Tex->Release();
        return false;
    }

    if(FAILED(hErr = res->GetSharedHandle(&sharedHandle2)))
    {
        RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Hook: res->GetSharedHandle2 failed, result = " << UINT(hErr) << endl;
        d3d11Tex->Release();
        res->Release();
        return false;
    }

	d3d11Tex->Release();
    res->Release();

    return true;
}

bool DoD3D11Hook(ID3D11Device *device)
{
    HRESULT hErr;

    D3D11_TEXTURE2D_DESC texGameDesc;
    ZeroMemory(&texGameDesc, sizeof(texGameDesc));
    texGameDesc.Width               = d3d11CaptureInfo.cx;
    texGameDesc.Height              = d3d11CaptureInfo.cy;
    texGameDesc.MipLevels           = 1;
    texGameDesc.ArraySize           = 1;
    texGameDesc.Format              = dxgiFormat;
    texGameDesc.SampleDesc.Count    = 1;
    texGameDesc.BindFlags           = D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    texGameDesc.Usage               = D3D11_USAGE_DEFAULT;
    texGameDesc.MiscFlags           = D3D11_RESOURCE_MISC_SHARED;

    ID3D11Texture2D *d3d11Tex;
    if(FAILED(hErr = device->CreateTexture2D(&texGameDesc, NULL, &d3d11Tex)))
    {
        RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Hook: creation of intermediary texture failed, result = " << UINT(hErr) << endl;
        return false;
    }

    if(FAILED(hErr = d3d11Tex->QueryInterface(__uuidof(ID3D11Resource), (void**)&copyTextureGame)))
    {
        RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Hook: d3d11Tex->QueryInterface(ID3D11Resource) failed, result = " << UINT(hErr) << endl;
        d3d11Tex->Release();
        return false;
    }

    IDXGIResource *res;
    if(FAILED(hErr = d3d11Tex->QueryInterface(IID_IDXGIResource, (void**)&res)))
    {
        RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Hook: d3d11Tex->QueryInterface(IID_IDXGIResource) failed, result = " << UINT(hErr) << endl;
        d3d11Tex->Release();
        return false;
    }

    if(FAILED(hErr = res->GetSharedHandle(&sharedHandle)))
    {
        RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Hook: res->GetSharedHandle failed, result = " << UINT(hErr) << endl;
        d3d11Tex->Release();
        res->Release();
        return false;
    }

    d3d11Tex->Release();
    res->Release();

    return true;
}

UINT STDMETHODCALLTYPE D3D11DeviceReleaseHook(ID3D10Device *device)
{
    /*device->AddRef();
    ULONG refVal = (*(RELEASEPROC)oldD3D11Release)(device);

    if(bHasTextures)
    {
        if(refVal == 8) //our two textures are holding the reference up, so always clear at 3
        {
            ClearD3D11Data();
            lpCurrentDevice = NULL;
            bTargetAcquired = false;
        }
    }
    else if(refVal == 1)
    {
        lpCurrentDevice = NULL;
        bTargetAcquired = false;
    }*/

    ULONG refVal = (*(RELEASEPROC)oldD3D11Release)(device);
    return refVal;
}

void DoD3D11Capture(IDXGISwapChain *swap)
{
    HRESULT hRes;

    ID3D11Device *device = NULL;
    if(SUCCEEDED(hRes = swap->GetDevice(__uuidof(ID3D11Device), (void**)&device)))
    {
        if(bCapturing && WaitForSingleObject(hSignalEnd, 0) == WAIT_OBJECT_0)
            bStopRequested = true;

        if(bCapturing && !IsWindow(hwndOBS))
        {
            hwndOBS = NULL;
            bStopRequested = true;
        }

        if(!lpCurrentDevice)
        {
            lpCurrentDevice = device;

            /*FARPROC curRelease = GetVTable(device, (8/4));
            if(curRelease != newD3D11Release)
            {
                oldD3D11Release = curRelease;
                newD3D11Release = (FARPROC)DeviceReleaseHook;
                SetVTable(device, (8/4), newD3D11Release);
            }*/
        }

        ID3D11DeviceContext *context;
        device->GetImmediateContext(&context);


		// CQ added ---------------------------------------
		// 
		// ID3D11Texture2D -> 
//		ID3D11DepthStencilState* currentStencilState = nullptr;
//		UINT stencilRef = 0;
//		context->OMGetDepthStencilState(&currentStencilState, &stencilRef);
//
//		// TODO cq get the depth buffer here
//
//		context->CopyResource(pDepthBufferCopy, pDepthBuffer);
//
//		D3D11_MAPPED_SUBRESOURCE mappedResource;
//
//		HRESULT mapResult = context->Map(pDepthBufferCopy, 0, D3D11_MAP_READ, 0, &mappedResource);
//
//		BYTE* pYourBytes = (BYTE*)mappedResource.pData;
//		unsigned int uiPitch = mappedResource.RowPitch;
//		 end CQ ---------------------------
//		 CQ Start custom depth buffer
//		 create the depth buffer texture
//		D3D11_TEXTURE2D_DESC texd;
//		ZeroMemory(&texd, sizeof(texd));
//
//		texd.Width = cq_width;
//		texd.Height = cq_height;
//		texd.ArraySize = 1;
//		texd.MipLevels = 1;
//		texd.SampleDesc.Count = 1;
//		texd.Format = DXGI_FORMAT_D32_FLOAT;
//		texd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
//
//		device->CreateTexture2D(&texd, NULL, &cq_pDepthBuffer);
//		// create the depth buffer copy texture
//		D3D11_TEXTURE2D_DESC texdCopy;
//		ZeroMemory(&texdCopy, sizeof(texdCopy));
//
//		texdCopy.Width = cq_width;
//		texdCopy.Height = cq_height;
//		texdCopy.ArraySize = 1;
//		texdCopy.MipLevels = 1;
//		texdCopy.SampleDesc.Count = 1; // Must have BindFlags set if greater than 1.
//		texdCopy.Format = DXGI_FORMAT_R32_TYPELESS;
//		texdCopy.BindFlags = NULL;
//		texdCopy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
//		texdCopy.Usage = D3D11_USAGE_STAGING; // Must be staging to read from CPU, but means BindFlags cannot be set.
//
//		//	pDepthBuffer->GetPrivateData()
//		device->CreateTexture2D(&texdCopy, NULL, &cq_pDepthBufferCopy);
//		// create the depth buffer
//		D3D11_DEPTH_STENCIL_VIEW_DESC dsvd;
//		ZeroMemory(&dsvd, sizeof(dsvd));
//
//		dsvd.Format = DXGI_FORMAT_D32_FLOAT;
//		dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
//
//		device->CreateDepthStencilView(cq_pDepthBuffer, &dsvd, &cq_zbuffer);
//		//	pDepthBuffer->Release();
//
//		// get the address of the back buffer
//		ID3D11Texture2D *pBackBuffer;
//		swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
//
//		// use the back buffer address to create the render target
//		device->CreateRenderTargetView(pBackBuffer, NULL, &cq_backbuffer);
//		pBackBuffer->Release();
//
//		// set the render target as the back buffer
//		context->OMSetRenderTargets(1, &cq_backbuffer, cq_zbuffer);
//		 CQ  End custom depth buffer
//		 CQ start capture test to file
//		context->CopyResource(cq_pDepthBufferCopy, cq_pDepthBuffer);
//
//		D3D11_MAPPED_SUBRESOURCE mappedResource;
//
//		HRESULT mapResult = context->Map(cq_pDepthBufferCopy, 0, D3D11_MAP_READ, 0, &mappedResource);
//
//		BYTE* pYourBytes = (BYTE*)mappedResource.pData;
//		unsigned int uiPitch = mappedResource.RowPitch;
//
//		if (cq_shouldWriteFile)
//		{
//			std::ofstream myfile;
//			myfile.open("cqdepth.txt");
//			for (int i = 0; i < uiPitch; i++)
//			{
//				// Write the first line of dexels
//				myfile << pYourBytes[i];
//			}
//			myfile << "\r\n";
//			myfile.close();
//			cq_shouldWriteFile = false;
//		}
		//// CQ end capture test to file

        if(bCapturing && bStopRequested)
        {
            RUNEVERYRESET logOutput << CurrentTimeString() << "stop requested, terminating d3d11 capture" << endl;

            ClearD3D11Data();
            bCapturing = false;
            bStopRequested = false;
        }

        if(!bCapturing && WaitForSingleObject(hSignalRestart, 0) == WAIT_OBJECT_0)
        {
            hwndOBS = FindWindow(OBS_WINDOW_CLASS, NULL);
            if(hwndOBS)
                bCapturing = true;
        }

        if(!bHasTextures && bCapturing)
        {
            if(dxgiFormat && hwndOBS)
            {
                BOOL bSuccess = DoD3D11Hook(device);

                if(bSuccess)
                {
                    d3d11CaptureInfo.mapID = InitializeSharedMemoryGPUCapture(&texData);
                    if(!d3d11CaptureInfo.mapID)
                    {
                        RUNEVERYRESET logOutput << CurrentTimeString() << "SwapPresentHook: creation of shared memory failed" << endl;
                        bSuccess = false;
                    }
                }

                if(bSuccess)
                {
                    bHasTextures = true;
                    d3d11CaptureInfo.captureType = CAPTURETYPE_SHAREDTEX;
                    d3d11CaptureInfo.bFlip = FALSE;
                    texData->texHandle = (DWORD)sharedHandle;

                    memcpy(infoMem, &d3d11CaptureInfo, sizeof(CaptureInfo));
                    SetEvent(hSignalReady);

                    logOutput << CurrentTimeString() << "DoD3D11Hook: success" << endl;
					logOutput << CurrentTimeString() << "cqcqcqcqcqcqcqcq setting shouldCreateSharedMem cqcqcqcqcqcqcqcq" << endl;
					shouldCreateSharedMem = true;
                }
                else
                {
                    ClearD3D11Data();
                }
            }
        }

        LONGLONG timeVal = OSGetTimeMicroseconds();

        //check keep alive state, dumb but effective
        if(bCapturing)
        {
            if (!keepAliveTime)
                keepAliveTime = timeVal;

            if((timeVal-keepAliveTime) > 5000000)
            {
                HANDLE hKeepAlive = OpenEvent(EVENT_ALL_ACCESS, FALSE, strKeepAlive.c_str());
                if (hKeepAlive) {
                    CloseHandle(hKeepAlive);
                } else {
                    ClearD3D11Data();
                    logOutput << CurrentTimeString() << "Keepalive no longer found on d3d11, freeing capture data" << endl;
                    bCapturing = false;
                }

                keepAliveTime = timeVal;
            }
        }

        if(bHasTextures)
        {
            LONGLONG frameTime;
            if(bCapturing)
            {
                if(texData)
                {
                    if(frameTime = texData->frameTime)
                    {
                        LONGLONG timeElapsed = timeVal-lastTime;

                        if(timeElapsed >= frameTime)
                        {
                            lastTime += frameTime;
                            if(timeElapsed > frameTime*2)
                                lastTime = timeVal;

                            DWORD nextCapture = curCapture == 0 ? 1 : 0;

                            ID3D11Resource *backBuffer = NULL;

                            if(SUCCEEDED(hRes = swap->GetBuffer(0, IID_ID3D11Resource, (void**)&backBuffer)))
                            {
                                if(bIsMultisampled)
                                {
	                                context->ResolveSubresource(copyTextureGame, 0, backBuffer, 0, dxgiFormat);
                                }

                                else
								{
								    context->CopyResource(copyTextureGame, backBuffer);
								}

                                RUNEVERYRESET logOutput << CurrentTimeString() << "successfully capturing d3d11 frames via GPU" << endl;

                                backBuffer->Release();
                            } else {
                                RUNEVERYRESET logOutput << CurrentTimeString() << "DoD3D11Capture: swap->GetBuffer failed: result = " << UINT(hRes) << endl;
                            }

                            curCapture = nextCapture;
                        }
                    }
                }
            } else {
                RUNEVERYRESET logOutput << CurrentTimeString() << "no longer capturing, terminating d3d11 capture" << endl;
                ClearD3D11Data();
            }
        }

        device->Release();
        context->Release();
    }
}

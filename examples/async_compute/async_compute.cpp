/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include <donut/app/ApplicationBase.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/BindingCache.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/utils.h>

#include <queue>
#include <chrono>


using namespace donut;

static const char* g_WindowTitle = "Donut Example: Async Compute";

class TextureQueue
{
private:
    std::queue<std::pair<nvrhi::TextureHandle, uint64_t>> m_Queue;
    std::mutex m_Mutex;

public:

    void Push(nvrhi::TextureHandle&& texture, uint64_t lastUse)
    {
	    std::lock_guard lock(m_Mutex);
        m_Queue.emplace(std::move(texture), lastUse);
    }

    bool TryPop(nvrhi::TextureHandle& outTexture, uint64_t& outLastUse)
    {
	    std::lock_guard lock(m_Mutex);

	    if (m_Queue.empty())
			return false;

        auto& [texture, lastUse] = m_Queue.front();
        outTexture.Swap(texture);
        outLastUse = lastUse;

        m_Queue.pop();

        return true;
    }

};

class AsyncCompute : public app::IRenderPass
{
private:
    nvrhi::ShaderHandle m_VertexShader;
    nvrhi::ShaderHandle m_PixelShader;
    nvrhi::ShaderHandle m_ComputeShader;

    nvrhi::BindingLayoutHandle m_DrawBindingLayout;
    nvrhi::BindingLayoutHandle m_ComputeBindingLayout;

    nvrhi::GraphicsPipelineHandle m_GraphicsPipeline;
    nvrhi::ComputePipelineHandle m_ComputePipeline;

    std::shared_ptr<engine::BindingCache> m_DrawBindings;
    std::shared_ptr<engine::BindingCache> m_ComputeBindings;

    nvrhi::CommandListLifetimeTrackerHandle m_CommandListLifetimeTracker;

    nvrhi::CommandListHandle m_DrawCommandList;
    nvrhi::CommandListHandle m_ComputeCommandList;

    std::thread m_ComputeThread;
    std::atomic_bool m_Terminate = false;

    TextureQueue m_RenderToComputeQueue;
    TextureQueue m_ComputeToRenderQueue;

    nvrhi::TextureHandle m_CurrentRenderTexture;
    nvrhi::SamplerHandle m_Sampler;
    uint64_t m_LastRenderTextureUse = 0;

public:
    using IRenderPass::IRenderPass;

    ~AsyncCompute() override
    {
        m_Terminate = true;
	    m_ComputeThread.join();
    }

    bool Init()
    {
        std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/async_compute" /  app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

        auto nativeFS = std::make_shared<vfs::NativeFileSystem>();
        engine::ShaderFactory shaderFactory(GetDevice(), nativeFS, appShaderPath);

        m_VertexShader = shaderFactory.CreateShader("shaders.hlsl", "main_vs", nullptr, nvrhi::ShaderType::Vertex);
        m_PixelShader = shaderFactory.CreateShader("shaders.hlsl", "main_ps", nullptr, nvrhi::ShaderType::Pixel);
        m_ComputeShader = shaderFactory.CreateShader("shaders.hlsl", "main_cs", nullptr, nvrhi::ShaderType::Compute);

        if (!m_VertexShader || !m_PixelShader || !m_ComputeShader)
        {
            return false;
        }

        m_Sampler = GetDevice()->createSampler({});

        {
	        nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc
        		.setVisibility(nvrhi::ShaderType::Pixel)
				.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            m_DrawBindingLayout = GetDevice()->createBindingLayout(layoutDesc);
        }
        {
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc
                .setVisibility(nvrhi::ShaderType::Compute)
				.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(uint32_t)))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
            m_ComputeBindingLayout = GetDevice()->createBindingLayout(layoutDesc);
        }

        nvrhi::ComputePipelineDesc psoDesc;
        psoDesc
			.setComputeShader(m_ComputeShader)
			.addBindingLayout(m_ComputeBindingLayout);
        m_ComputePipeline = GetDevice()->createComputePipeline(psoDesc);

        m_DrawBindings = std::make_shared<engine::BindingCache>(GetDevice());
        m_ComputeBindings = std::make_shared<engine::BindingCache>(GetDevice());

        m_CommandListLifetimeTracker = GetDevice()->createCommandListLifetimeTracker(nvrhi::CommandQueue::Compute);

        m_DrawCommandList = GetDevice()->createCommandList();
        nvrhi::CommandListParameters params;
        params
    		.setEnableImmediateExecution(false)
			.setQueueType(nvrhi::CommandQueue::Compute)
    		.setLifetimeTracker(m_CommandListLifetimeTracker);
        m_ComputeCommandList = GetDevice()->createCommandList(params);

        nvrhi::TextureDesc texDesc;
        texDesc
    		.setFormat(nvrhi::Format::RGBA8_UNORM)
			.setWidth(512)
			.setHeight(512)
    		.setIsUAV(true)
    		.enableAutomaticStateTracking(nvrhi::ResourceStates::NonPixelShaderResource);

        constexpr size_t NumTextures = 2;
        for (size_t i = 0; i < NumTextures; i++)
        {
	        m_RenderToComputeQueue.Push(GetDevice()->createTexture(texDesc), 0);
        }

        m_ComputeThread = std::thread([this](){ this->AsyncThreadProc(); });

        return true;
    }

    void BackBufferResizing() override
    { 
        m_GraphicsPipeline = nullptr;
    }

    void Animate(float fElapsedTimeSeconds) override
    {
        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle);
    }
    
    void Render(nvrhi::IFramebuffer* framebuffer) override
    {
        if (!m_GraphicsPipeline)
        {
            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.VS = m_VertexShader;
            psoDesc.PS = m_PixelShader;
            psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
            psoDesc.renderState.depthStencilState.depthTestEnable = false;
            psoDesc.bindingLayouts = { m_DrawBindingLayout };

            m_GraphicsPipeline = GetDevice()->createGraphicsPipeline(psoDesc, framebuffer->getFramebufferInfo());
        }

        nvrhi::TextureHandle newTexture;
        uint64_t newTextureLastUse;
        if (m_ComputeToRenderQueue.TryPop(newTexture, newTextureLastUse))
        {
	        m_CurrentRenderTexture.Swap(newTexture);
            if (newTexture)
            {
				m_RenderToComputeQueue.Push(std::move(newTexture), m_LastRenderTextureUse);
            }

            GetDevice()->queueWaitForCommandList(nvrhi::CommandQueue::Graphics, nvrhi::CommandQueue::Compute, newTextureLastUse);
        }

        m_DrawCommandList->open();

        nvrhi::utils::ClearColorAttachment(m_DrawCommandList, framebuffer, 0, nvrhi::Color(0.f));

        if (m_CurrentRenderTexture)
        {
            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_CurrentRenderTexture));
            bindingDesc.addItem(nvrhi::BindingSetItem::Sampler(0, m_Sampler));
            nvrhi::BindingSetHandle bindings = m_DrawBindings->GetOrCreateBindingSet(bindingDesc, m_DrawBindingLayout);

            nvrhi::GraphicsState state;
            state.pipeline = m_GraphicsPipeline;
            state.bindings = { bindings };
            state.framebuffer = framebuffer;
            state.viewport.addViewportAndScissorRect(framebuffer->getFramebufferInfo().getViewport());

            m_DrawCommandList->setGraphicsState(state);

            nvrhi::DrawArguments args;
            args.vertexCount = 4;
            m_DrawCommandList->draw(args);
        }

        m_DrawCommandList->close();
        m_LastRenderTextureUse = GetDevice()->executeCommandList(m_DrawCommandList);
    }

    void AsyncThreadProc()
    {
		uint32_t counter = 0;

		using clock = std::chrono::steady_clock;
        constexpr std::chrono::microseconds intervalMicroseconds{ 10000 }; // 100Hz

	    while (!m_Terminate)
	    {
			clock::time_point nextTimePoint = clock::now() + intervalMicroseconds;
            m_CommandListLifetimeTracker->runGarbageCollection();

		    nvrhi::TextureHandle texture;
            uint64_t textureLastUse = 0;
            while (!m_Terminate && !m_RenderToComputeQueue.TryPop(texture, textureLastUse))
            {}

            if (m_Terminate)
				break;

            m_ComputeCommandList->open();

            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(0, texture));
            bindingDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(uint32_t)));
            nvrhi::BindingSetHandle bindings = m_ComputeBindings->GetOrCreateBindingSet(bindingDesc, m_ComputeBindingLayout);

            nvrhi::ComputeState state;
            state.pipeline = m_ComputePipeline;
            state.bindings = { bindings };
            m_ComputeCommandList->setComputeState(state);

            m_ComputeCommandList->setPushConstants(&counter, sizeof(counter));

            m_ComputeCommandList->dispatch(64, 64);

            m_ComputeCommandList->close();

            if (textureLastUse > 0)
				GetDevice()->queueWaitForCommandList(nvrhi::CommandQueue::Compute, nvrhi::CommandQueue::Graphics, textureLastUse);
            textureLastUse = GetDevice()->executeCommandList(m_ComputeCommandList, nvrhi::CommandQueue::Compute);

            m_ComputeToRenderQueue.Push(std::move(texture), textureLastUse);

            counter++;
            std::this_thread::sleep_until(nextTimePoint);
	    }
    }

};

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    app::DeviceManager* deviceManager = app::DeviceManager::Create(api);

    app::DeviceCreationParameters deviceParams;
    deviceParams.enableComputeQueue = true;
#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true; 
    deviceParams.enableNvrhiValidationLayer = true;
#endif

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle))
    {
        log::fatal("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }
    
    {
        AsyncCompute example(deviceManager);
        if (example.Init())
        {
            deviceManager->AddRenderPassToBack(&example);
            deviceManager->RunMessageLoop();
            deviceManager->RemoveRenderPass(&example);
        }
    }
    
    deviceManager->Shutdown();

    delete deviceManager;

    return 0;
}

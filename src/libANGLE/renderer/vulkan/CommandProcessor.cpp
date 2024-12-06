//
// Copyright 2020 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CommandProcessor.cpp:
//    Implements the class methods for CommandProcessor.
//

#include "libANGLE/renderer/vulkan/CommandProcessor.h"
#include "common/system_utils.h"
#include "libANGLE/renderer/vulkan/SyncVk.h"
#include "libANGLE/renderer/vulkan/vk_renderer.h"

namespace rx
{
namespace vk
{
namespace
{
constexpr bool kOutputVmaStatsString = false;
// When suballocation garbages is more than this, we may wait for GPU to finish and free up some
// memory for allocation.
constexpr VkDeviceSize kMaxBufferSuballocationGarbageSize = 64 * 1024 * 1024;

void InitializeSubmitInfo(VkSubmitInfo *submitInfo,
                          const PrimaryCommandBuffer &commandBuffer,
                          const std::vector<VkSemaphore> &waitSemaphores,
                          const std::vector<VkPipelineStageFlags> &waitSemaphoreStageMasks,
                          const VkSemaphore &signalSemaphore)
{
    // Verify that the submitInfo has been zero'd out.
    ASSERT(submitInfo->signalSemaphoreCount == 0);
    ASSERT(waitSemaphores.size() == waitSemaphoreStageMasks.size());
    submitInfo->sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo->commandBufferCount = commandBuffer.valid() ? 1 : 0;
    submitInfo->pCommandBuffers    = commandBuffer.ptr();
    submitInfo->waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
    submitInfo->pWaitSemaphores    = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
    submitInfo->pWaitDstStageMask  = waitSemaphoreStageMasks.data();

    if (signalSemaphore != VK_NULL_HANDLE)
    {
        submitInfo->signalSemaphoreCount = 1;
        submitInfo->pSignalSemaphores    = &signalSemaphore;
    }
}

void GetDeviceQueue(VkDevice device,
                    bool makeProtected,
                    uint32_t queueFamilyIndex,
                    uint32_t queueIndex,
                    VkQueue *queue)
{
    if (makeProtected)
    {
        VkDeviceQueueInfo2 queueInfo2 = {};
        queueInfo2.sType              = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
        queueInfo2.flags              = VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;
        queueInfo2.queueFamilyIndex   = queueFamilyIndex;
        queueInfo2.queueIndex         = queueIndex;

        vkGetDeviceQueue2(device, &queueInfo2, queue);
    }
    else
    {
        vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, queue);
    }
}
}  // namespace

// RecyclableFence implementation
RecyclableFence::RecyclableFence() : mRecycler(nullptr) {}

RecyclableFence::~RecyclableFence()
{
    ASSERT(!valid());
}

VkResult RecyclableFence::init(VkDevice device, FenceRecycler *recycler)
{
    ASSERT(!valid());
    ASSERT(mRecycler == nullptr);

    // First try to fetch from recycler. If that failed, try to create a new VkFence
    recycler->fetch(device, &mFence);
    if (!valid())
    {
        VkFenceCreateInfo fenceCreateInfo = {};
        fenceCreateInfo.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCreateInfo.flags             = 0;
        VkResult result                   = mFence.init(device, fenceCreateInfo);
        if (result != VK_SUCCESS)
        {
            ASSERT(!valid());
            return result;
        }
        ASSERT(valid());
    }

    mRecycler = recycler;

    return VK_SUCCESS;
}

void RecyclableFence::destroy(VkDevice device)
{
    if (valid())
    {
        if (mRecycler != nullptr)
        {
            mRecycler->recycle(std::move(mFence));
        }
        else
        {
            // Recycler was detached - destroy the fence.
            mFence.destroy(device);
        }
        ASSERT(!valid());
    }
}

// FenceRecycler implementation
void FenceRecycler::destroy(Context *context)
{
    std::lock_guard<angle::SimpleMutex> lock(mMutex);
    mRecycler.destroy(context->getDevice());
}

void FenceRecycler::fetch(VkDevice device, Fence *fenceOut)
{
    ASSERT(fenceOut != nullptr && !fenceOut->valid());
    std::lock_guard<angle::SimpleMutex> lock(mMutex);
    if (!mRecycler.empty())
    {
        mRecycler.fetch(fenceOut);
        fenceOut->reset(device);
    }
}

void FenceRecycler::recycle(Fence &&fence)
{
    std::lock_guard<angle::SimpleMutex> lock(mMutex);
    mRecycler.recycle(std::move(fence));
}

// CommandProcessorTask implementation
void CommandProcessorTask::initTask()
{
    mTask                           = CustomTask::Invalid;
    mOutsideRenderPassCommandBuffer = nullptr;
    mRenderPassCommandBuffer        = nullptr;
    mSemaphore                      = VK_NULL_HANDLE;
    mOneOffWaitSemaphore            = VK_NULL_HANDLE;
    mOneOffWaitSemaphoreStageMask   = 0;
    mPresentInfo                    = {};
    mPresentInfo.pResults           = nullptr;
    mPresentInfo.pSwapchains        = nullptr;
    mPresentInfo.pImageIndices      = nullptr;
    mPresentInfo.pNext              = nullptr;
    mPresentInfo.pWaitSemaphores    = nullptr;
    mPresentFence                   = VK_NULL_HANDLE;
    mSwapchainStatus                = nullptr;
    mOneOffCommandBuffer            = VK_NULL_HANDLE;
    mPriority                       = egl::ContextPriority::Medium;
    mProtectionType                 = ProtectionType::InvalidEnum;
}

void CommandProcessorTask::initFlushWaitSemaphores(
    ProtectionType protectionType,
    egl::ContextPriority priority,
    std::vector<VkSemaphore> &&waitSemaphores,
    std::vector<VkPipelineStageFlags> &&waitSemaphoreStageMasks)
{
    mTask                    = CustomTask::FlushWaitSemaphores;
    mPriority                = priority;
    mProtectionType          = protectionType;
    mWaitSemaphores          = std::move(waitSemaphores);
    mWaitSemaphoreStageMasks = std::move(waitSemaphoreStageMasks);
}

void CommandProcessorTask::initOutsideRenderPassProcessCommands(
    ProtectionType protectionType,
    egl::ContextPriority priority,
    OutsideRenderPassCommandBufferHelper *commandBuffer)
{
    mTask                           = CustomTask::ProcessOutsideRenderPassCommands;
    mOutsideRenderPassCommandBuffer = commandBuffer;
    mPriority                       = priority;
    mProtectionType                 = protectionType;
}

void CommandProcessorTask::initRenderPassProcessCommands(
    ProtectionType protectionType,
    egl::ContextPriority priority,
    RenderPassCommandBufferHelper *commandBuffer,
    const RenderPass *renderPass,
    VkFramebuffer framebufferOverride)
{
    mTask                    = CustomTask::ProcessRenderPassCommands;
    mRenderPassCommandBuffer = commandBuffer;
    mPriority                = priority;
    mProtectionType          = protectionType;

    mRenderPass.setHandle(renderPass->getHandle());
    mFramebufferOverride = framebufferOverride;
}

void CommandProcessorTask::copyPresentInfo(const VkPresentInfoKHR &other)
{
    if (other.sType == 0)
    {
        return;
    }

    mPresentInfo.sType = other.sType;
    mPresentInfo.pNext = nullptr;

    if (other.swapchainCount > 0)
    {
        ASSERT(other.swapchainCount == 1);
        mPresentInfo.swapchainCount = 1;
        mSwapchain                  = other.pSwapchains[0];
        mPresentInfo.pSwapchains    = &mSwapchain;
        mImageIndex                 = other.pImageIndices[0];
        mPresentInfo.pImageIndices  = &mImageIndex;
    }

    if (other.waitSemaphoreCount > 0)
    {
        ASSERT(other.waitSemaphoreCount == 1);
        mPresentInfo.waitSemaphoreCount = 1;
        mWaitSemaphore                  = other.pWaitSemaphores[0];
        mPresentInfo.pWaitSemaphores    = &mWaitSemaphore;
    }

    mPresentInfo.pResults = other.pResults;

    void *pNext = const_cast<void *>(other.pNext);
    while (pNext != nullptr)
    {
        VkStructureType sType = *reinterpret_cast<VkStructureType *>(pNext);
        switch (sType)
        {
            case VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR:
            {
                const VkPresentRegionsKHR *presentRegions =
                    reinterpret_cast<VkPresentRegionsKHR *>(pNext);
                mPresentRegion = *presentRegions->pRegions;
                mRects.resize(mPresentRegion.rectangleCount);
                for (uint32_t i = 0; i < mPresentRegion.rectangleCount; i++)
                {
                    mRects[i] = presentRegions->pRegions->pRectangles[i];
                }
                mPresentRegion.pRectangles = mRects.data();

                mPresentRegions.sType          = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR;
                mPresentRegions.pNext          = nullptr;
                mPresentRegions.swapchainCount = 1;
                mPresentRegions.pRegions       = &mPresentRegion;
                AddToPNextChain(&mPresentInfo, &mPresentRegions);
                pNext = const_cast<void *>(presentRegions->pNext);
                break;
            }
            case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT:
            {
                const VkSwapchainPresentFenceInfoEXT *presentFenceInfo =
                    reinterpret_cast<VkSwapchainPresentFenceInfoEXT *>(pNext);
                ASSERT(presentFenceInfo->swapchainCount == 1);
                mPresentFence = presentFenceInfo->pFences[0];

                mPresentFenceInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
                mPresentFenceInfo.pNext = nullptr;
                mPresentFenceInfo.swapchainCount = 1;
                mPresentFenceInfo.pFences        = &mPresentFence;
                AddToPNextChain(&mPresentInfo, &mPresentFenceInfo);
                pNext = const_cast<void *>(presentFenceInfo->pNext);
                break;
            }
            case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT:
            {
                const VkSwapchainPresentModeInfoEXT *presentModeInfo =
                    reinterpret_cast<VkSwapchainPresentModeInfoEXT *>(pNext);
                ASSERT(presentModeInfo->swapchainCount == 1);
                mPresentMode = presentModeInfo->pPresentModes[0];

                mPresentModeInfo.sType          = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT;
                mPresentModeInfo.pNext          = nullptr;
                mPresentModeInfo.swapchainCount = 1;
                mPresentModeInfo.pPresentModes  = &mPresentMode;
                AddToPNextChain(&mPresentInfo, &mPresentModeInfo);
                pNext = const_cast<void *>(presentModeInfo->pNext);
                break;
            }
            default:
                ERR() << "Unknown sType: " << sType << " in VkPresentInfoKHR.pNext chain";
                UNREACHABLE();
                break;
        }
    }
}

void CommandProcessorTask::initPresent(egl::ContextPriority priority,
                                       const VkPresentInfoKHR &presentInfo,
                                       SwapchainStatus *swapchainStatus)
{
    mTask            = CustomTask::Present;
    mPriority        = priority;
    mSwapchainStatus = swapchainStatus;
    copyPresentInfo(presentInfo);
}

void CommandProcessorTask::initFlushAndQueueSubmit(VkSemaphore semaphore,
                                                   SharedExternalFence &&externalFence,
                                                   ProtectionType protectionType,
                                                   egl::ContextPriority priority,
                                                   const QueueSerial &submitQueueSerial)
{
    mTask              = CustomTask::FlushAndQueueSubmit;
    mSemaphore         = semaphore;
    mExternalFence     = std::move(externalFence);
    mPriority          = priority;
    mProtectionType    = protectionType;
    mSubmitQueueSerial = submitQueueSerial;
}

void CommandProcessorTask::initOneOffQueueSubmit(VkCommandBuffer commandBufferHandle,
                                                 ProtectionType protectionType,
                                                 egl::ContextPriority priority,
                                                 VkSemaphore waitSemaphore,
                                                 VkPipelineStageFlags waitSemaphoreStageMask,
                                                 const QueueSerial &submitQueueSerial)
{
    mTask                         = CustomTask::OneOffQueueSubmit;
    mOneOffCommandBuffer          = commandBufferHandle;
    mOneOffWaitSemaphore          = waitSemaphore;
    mOneOffWaitSemaphoreStageMask = waitSemaphoreStageMask;
    mPriority                     = priority;
    mProtectionType               = protectionType;
    mSubmitQueueSerial            = submitQueueSerial;
}

CommandProcessorTask &CommandProcessorTask::operator=(CommandProcessorTask &&rhs)
{
    if (this == &rhs)
    {
        return *this;
    }

    std::swap(mRenderPass, rhs.mRenderPass);
    std::swap(mFramebufferOverride, rhs.mFramebufferOverride);
    std::swap(mOutsideRenderPassCommandBuffer, rhs.mOutsideRenderPassCommandBuffer);
    std::swap(mRenderPassCommandBuffer, rhs.mRenderPassCommandBuffer);
    std::swap(mTask, rhs.mTask);
    std::swap(mWaitSemaphores, rhs.mWaitSemaphores);
    std::swap(mWaitSemaphoreStageMasks, rhs.mWaitSemaphoreStageMasks);
    std::swap(mSemaphore, rhs.mSemaphore);
    std::swap(mExternalFence, rhs.mExternalFence);
    std::swap(mOneOffWaitSemaphore, rhs.mOneOffWaitSemaphore);
    std::swap(mOneOffWaitSemaphoreStageMask, rhs.mOneOffWaitSemaphoreStageMask);
    std::swap(mSubmitQueueSerial, rhs.mSubmitQueueSerial);
    std::swap(mPriority, rhs.mPriority);
    std::swap(mProtectionType, rhs.mProtectionType);
    std::swap(mOneOffCommandBuffer, rhs.mOneOffCommandBuffer);

    copyPresentInfo(rhs.mPresentInfo);
    std::swap(mSwapchainStatus, rhs.mSwapchainStatus);

    // clear rhs now that everything has moved.
    rhs.initTask();

    return *this;
}

// CommandBatch implementation.
CommandBatch::CommandBatch()
    : mProtectionType(ProtectionType::InvalidEnum), mCommandPoolAccess(nullptr)
{}

CommandBatch::~CommandBatch() = default;

CommandBatch::CommandBatch(CommandBatch &&other) : CommandBatch()
{
    *this = std::move(other);
}

CommandBatch &CommandBatch::operator=(CommandBatch &&other)
{
    std::swap(mQueueSerial, other.mQueueSerial);
    std::swap(mProtectionType, other.mProtectionType);
    std::swap(mPrimaryCommands, other.mPrimaryCommands);
    std::swap(mCommandPoolAccess, other.mCommandPoolAccess);
    std::swap(mSecondaryCommands, other.mSecondaryCommands);
    std::swap(mFence, other.mFence);
    std::swap(mExternalFence, other.mExternalFence);
    return *this;
}

void CommandBatch::destroy(VkDevice device)
{
    if (mPrimaryCommands.valid())
    {
        ASSERT(mCommandPoolAccess != nullptr);
        mCommandPoolAccess->destroyPrimaryCommandBuffer(device, &mPrimaryCommands);
    }
    mSecondaryCommands.releaseCommandBuffers();
    if (mFence)
    {
        mFence->detachRecycler();
        mFence.reset();
    }
    mExternalFence.reset();
    // Do not clean other members to catch invalid reuse attempt with ASSERTs.
}

angle::Result CommandBatch::release(Context *context)
{
    if (mPrimaryCommands.valid())
    {
        ASSERT(mCommandPoolAccess != nullptr);
        ANGLE_TRY(mCommandPoolAccess->collectPrimaryCommandBuffer(context, mProtectionType,
                                                                  &mPrimaryCommands));
    }
    mSecondaryCommands.releaseCommandBuffers();
    mFence.reset();
    mExternalFence.reset();
    // Do not clean other members to catch invalid reuse attempt with ASSERTs.
    return angle::Result::Continue;
}

void CommandBatch::setQueueSerial(const QueueSerial &serial)
{
    ASSERT(serial.valid());
    ASSERT(!mQueueSerial.valid());
    mQueueSerial = serial;
}

void CommandBatch::setProtectionType(ProtectionType protectionType)
{
    ASSERT(protectionType != ProtectionType::InvalidEnum);
    ASSERT(mProtectionType == ProtectionType::InvalidEnum);
    mProtectionType = protectionType;
}

void CommandBatch::setPrimaryCommands(PrimaryCommandBuffer &&primaryCommands,
                                      CommandPoolAccess *commandPoolAccess)
{
    // primaryCommands is optional.
    ASSERT(!(primaryCommands.valid() && commandPoolAccess == nullptr));
    ASSERT(!mPrimaryCommands.valid());
    ASSERT(mCommandPoolAccess == nullptr);
    mPrimaryCommands   = std::move(primaryCommands);
    mCommandPoolAccess = commandPoolAccess;
}

void CommandBatch::setSecondaryCommands(SecondaryCommandBufferCollector &&secondaryCommands)
{
    // secondaryCommands is optional.
    ASSERT(mSecondaryCommands.empty());
    mSecondaryCommands = std::move(secondaryCommands);
}

VkResult CommandBatch::initFence(VkDevice device, FenceRecycler *recycler)
{
    ASSERT(!hasFence());
    auto fence            = SharedFence::MakeShared(device);
    const VkResult result = fence->init(device, recycler);
    if (result == VK_SUCCESS)
    {
        ASSERT(fence->valid());
        mFence = std::move(fence);
    }
    return result;
}

void CommandBatch::setExternalFence(SharedExternalFence &&externalFence)
{
    ASSERT(!hasFence());
    mExternalFence = std::move(externalFence);
}

const QueueSerial &CommandBatch::getQueueSerial() const
{
    ASSERT(mQueueSerial.valid());
    return mQueueSerial;
}

const PrimaryCommandBuffer &CommandBatch::getPrimaryCommands() const
{
    return mPrimaryCommands;
}

const SharedExternalFence &CommandBatch::getExternalFence()
{
    return mExternalFence;
}

bool CommandBatch::hasFence() const
{
    ASSERT(!mExternalFence || !mFence);
    ASSERT(!mFence || mFence->valid());
    return mFence || mExternalFence;
}

VkFence CommandBatch::getFenceHandle() const
{
    ASSERT(hasFence());
    return mFence ? mFence->get().getHandle() : mExternalFence->getHandle();
}

VkResult CommandBatch::getFenceStatus(VkDevice device) const
{
    ASSERT(hasFence());
    return mFence ? mFence->get().getStatus(device) : mExternalFence->getStatus(device);
}

VkResult CommandBatch::waitFence(VkDevice device, uint64_t timeout) const
{
    ASSERT(hasFence());
    return mFence ? mFence->get().wait(device, timeout) : mExternalFence->wait(device, timeout);
}

VkResult CommandBatch::waitFenceUnlocked(VkDevice device,
                                         uint64_t timeout,
                                         std::unique_lock<angle::SimpleMutex> *lock) const
{
    ASSERT(hasFence());
    VkResult status;
    // You can only use the local copy of the fence without lock.
    // Do not access "this" after unlock() because object might be deleted from other thread.
    if (mFence)
    {
        const SharedFence localFenceToWaitOn = mFence;
        lock->unlock();
        status = localFenceToWaitOn->get().wait(device, timeout);
        lock->lock();
    }
    else
    {
        const SharedExternalFence localFenceToWaitOn = mExternalFence;
        lock->unlock();
        status = localFenceToWaitOn->wait(device, timeout);
        lock->lock();
    }
    return status;
}

// CommandProcessor implementation.
void CommandProcessor::handleError(VkResult errorCode,
                                   const char *file,
                                   const char *function,
                                   unsigned int line)
{
    ASSERT(errorCode != VK_SUCCESS);

    std::stringstream errorStream;
    errorStream << "Internal Vulkan error (" << errorCode << "): " << VulkanResultString(errorCode)
                << ".";

    if (errorCode == VK_ERROR_DEVICE_LOST)
    {
        WARN() << errorStream.str();
        handleDeviceLost(mRenderer);
    }

    std::lock_guard<angle::SimpleMutex> queueLock(mErrorMutex);
    Error error = {errorCode, file, function, line};
    mErrors.emplace(error);
}

CommandProcessor::CommandProcessor(Renderer *renderer, CommandQueue *commandQueue)
    : Context(renderer),
      mTaskQueue(kMaxCommandProcessorTasksLimit),
      mCommandQueue(commandQueue),
      mTaskThreadShouldExit(false),
      mNeedCommandsAndGarbageCleanup(false)
{
    std::lock_guard<angle::SimpleMutex> queueLock(mErrorMutex);
    while (!mErrors.empty())
    {
        mErrors.pop();
    }
}

CommandProcessor::~CommandProcessor() = default;

angle::Result CommandProcessor::checkAndPopPendingError(Context *errorHandlingContext)
{
    std::lock_guard<angle::SimpleMutex> queueLock(mErrorMutex);
    if (mErrors.empty())
    {
        return angle::Result::Continue;
    }

    while (!mErrors.empty())
    {
        Error err = mErrors.front();
        mErrors.pop();
        errorHandlingContext->handleError(err.errorCode, err.file, err.function, err.line);
    }
    return angle::Result::Stop;
}

angle::Result CommandProcessor::queueCommand(CommandProcessorTask &&task)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "CommandProcessor::queueCommand");
    // Take mTaskEnqueueMutex lock. If task queue is full, try to drain one.
    std::unique_lock<std::mutex> enqueueLock(mTaskEnqueueMutex);
    if (mTaskQueue.full())
    {
        std::lock_guard<angle::SimpleMutex> dequeueLock(mTaskDequeueMutex);
        // Check mTasks again in case someone just drained the mTasks.
        if (mTaskQueue.full())
        {
            CommandProcessorTask frontTask(std::move(mTaskQueue.front()));
            mTaskQueue.pop();
            ANGLE_TRY(processTask(&frontTask));
        }
    }
    mTaskQueue.push(std::move(task));
    mWorkAvailableCondition.notify_one();

    return angle::Result::Continue;
}

void CommandProcessor::requestCommandsAndGarbageCleanup()
{
    if (!mNeedCommandsAndGarbageCleanup.exchange(true))
    {
        // request clean up in async thread
        std::unique_lock<std::mutex> enqueueLock(mTaskEnqueueMutex);
        mWorkAvailableCondition.notify_one();
    }
}

void CommandProcessor::processTasks()
{
    angle::SetCurrentThreadName("ANGLE-Submit");

    while (true)
    {
        bool exitThread      = false;
        angle::Result result = processTasksImpl(&exitThread);
        if (exitThread)
        {
            // We are doing a controlled exit of the thread, break out of the while loop.
            break;
        }
        if (result != angle::Result::Continue)
        {
            // TODO: https://issuetracker.google.com/issues/170311829 - follow-up on error handling
            // ContextVk::commandProcessorSyncErrorsAndQueueCommand and WindowSurfaceVk::destroy
            // do error processing, is anything required here? Don't think so, mostly need to
            // continue the worker thread until it's been told to exit.
            UNREACHABLE();
        }
    }
}

angle::Result CommandProcessor::processTasksImpl(bool *exitThread)
{
    while (true)
    {
        std::unique_lock<std::mutex> enqueueLock(mTaskEnqueueMutex);
        if (mTaskQueue.empty())
        {
            if (mTaskThreadShouldExit)
            {
                break;
            }

            // Only wake if notified and command queue is not empty
            mWorkAvailableCondition.wait(enqueueLock, [this] {
                return !mTaskQueue.empty() || mTaskThreadShouldExit ||
                       mNeedCommandsAndGarbageCleanup;
            });
        }
        // Do submission with mTaskEnqueueMutex unlocked so that we still allow enqueue while we
        // process work.
        enqueueLock.unlock();

        // Take submission lock to ensure the submission is in the same order as we received.
        std::lock_guard<angle::SimpleMutex> dequeueLock(mTaskDequeueMutex);
        if (!mTaskQueue.empty())
        {
            CommandProcessorTask task(std::move(mTaskQueue.front()));
            mTaskQueue.pop();

            // Artificially make the task take longer to catch threading issues.
            if (getFeatures().slowAsyncCommandQueueForTesting.enabled)
            {
                constexpr double kSlowdownTime = 0.005;

                double startTime = angle::GetCurrentSystemTime();
                while (angle::GetCurrentSystemTime() - startTime < kSlowdownTime)
                {
                    // Busy waiting
                }
            }

            ANGLE_TRY(processTask(&task));
        }

        if (mNeedCommandsAndGarbageCleanup.exchange(false))
        {
            // Always check completed commands again in case anything new has been finished.
            ANGLE_TRY(mCommandQueue->checkCompletedCommands(this));

            // Reset command buffer and clean up garbage
            if (mRenderer->isAsyncCommandBufferResetAndGarbageCleanupEnabled() &&
                mCommandQueue->hasFinishedCommands())
            {
                ANGLE_TRY(mCommandQueue->releaseFinishedCommands(this));
            }
            mRenderer->cleanupGarbage(nullptr);
        }
    }
    *exitThread = true;
    return angle::Result::Continue;
}

angle::Result CommandProcessor::processTask(CommandProcessorTask *task)
{
    switch (task->getTaskCommand())
    {
        case CustomTask::FlushAndQueueSubmit:
        {
            ANGLE_TRACE_EVENT0("gpu.angle", "processTask::FlushAndQueueSubmit");
            // End command buffer

            // Call submitCommands()
            ANGLE_TRY(mCommandQueue->submitCommands(
                this, task->getProtectionType(), task->getPriority(), task->getSemaphore(),
                std::move(task->getExternalFence()), task->getSubmitQueueSerial()));
            mNeedCommandsAndGarbageCleanup = true;
            break;
        }
        case CustomTask::OneOffQueueSubmit:
        {
            ANGLE_TRACE_EVENT0("gpu.angle", "processTask::OneOffQueueSubmit");

            ANGLE_TRY(mCommandQueue->queueSubmitOneOff(
                this, task->getProtectionType(), task->getPriority(),
                task->getOneOffCommandBuffer(), task->getOneOffWaitSemaphore(),
                task->getOneOffWaitSemaphoreStageMask(), SubmitPolicy::EnsureSubmitted,
                task->getSubmitQueueSerial()));
            mNeedCommandsAndGarbageCleanup = true;
            break;
        }
        case CustomTask::Present:
        {
            // Do not access task->getSwapchainStatus() after this call because it is marked as no
            // longer pending, and so may get deleted or clobbered by another thread.
            VkResult result =
                present(task->getPriority(), task->getPresentInfo(), task->getSwapchainStatus());

            // We get to ignore these as they are not fatal
            if (result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_SUBOPTIMAL_KHR &&
                result != VK_SUCCESS)
            {
                // Save the error so that we can handle it.
                // Don't leave processing loop, don't consider errors from present to be fatal.
                // TODO: https://issuetracker.google.com/issues/170329600 - This needs to improve to
                // properly parallelize present
                handleError(result, __FILE__, __FUNCTION__, __LINE__);
            }
            break;
        }
        case CustomTask::FlushWaitSemaphores:
        {
            mCommandQueue->flushWaitSemaphores(task->getProtectionType(), task->getPriority(),
                                               std::move(task->getWaitSemaphores()),
                                               std::move(task->getWaitSemaphoreStageMasks()));
            break;
        }
        case CustomTask::ProcessOutsideRenderPassCommands:
        {
            OutsideRenderPassCommandBufferHelper *commandBuffer =
                task->getOutsideRenderPassCommandBuffer();
            ANGLE_TRY(mCommandQueue->flushOutsideRPCommands(this, task->getProtectionType(),
                                                            task->getPriority(), &commandBuffer));

            OutsideRenderPassCommandBufferHelper *originalCommandBuffer =
                task->getOutsideRenderPassCommandBuffer();
            mRenderer->recycleOutsideRenderPassCommandBufferHelper(&originalCommandBuffer);
            break;
        }
        case CustomTask::ProcessRenderPassCommands:
        {
            RenderPassCommandBufferHelper *commandBuffer = task->getRenderPassCommandBuffer();
            ANGLE_TRY(mCommandQueue->flushRenderPassCommands(
                this, task->getProtectionType(), task->getPriority(), task->getRenderPass(),
                task->getFramebufferOverride(), &commandBuffer));

            RenderPassCommandBufferHelper *originalCommandBuffer =
                task->getRenderPassCommandBuffer();
            mRenderer->recycleRenderPassCommandBufferHelper(&originalCommandBuffer);
            break;
        }
        default:
            UNREACHABLE();
            break;
    }

    return angle::Result::Continue;
}

angle::Result CommandProcessor::waitForAllWorkToBeSubmitted(Context *context)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "CommandProcessor::waitForAllWorkToBeSubmitted");
    // Take mWorkerMutex lock so that no one is able to enqueue more work while we drain it
    // and handle device lost.
    std::lock_guard<std::mutex> enqueueLock(mTaskEnqueueMutex);
    std::lock_guard<angle::SimpleMutex> dequeueLock(mTaskDequeueMutex);
    // Sync any errors to the context
    // Do this inside the mutex to prevent new errors adding to the list.
    ANGLE_TRY(checkAndPopPendingError(context));

    while (!mTaskQueue.empty())
    {
        CommandProcessorTask task(std::move(mTaskQueue.front()));
        mTaskQueue.pop();
        ANGLE_TRY(processTask(&task));
    }

    if (mRenderer->isAsyncCommandBufferResetAndGarbageCleanupEnabled())
    {
        ANGLE_TRY(mCommandQueue->releaseFinishedCommands(context));
        mRenderer->cleanupGarbage(nullptr);
    }

    mNeedCommandsAndGarbageCleanup = false;

    return angle::Result::Continue;
}

angle::Result CommandProcessor::init()
{
    mTaskThread = std::thread(&CommandProcessor::processTasks, this);

    return angle::Result::Continue;
}

void CommandProcessor::destroy(Context *context)
{
    {
        // Request to terminate the worker thread
        std::lock_guard<std::mutex> enqueueLock(mTaskEnqueueMutex);
        mTaskThreadShouldExit = true;
        mWorkAvailableCondition.notify_one();
    }

    (void)waitForAllWorkToBeSubmitted(context);
    if (mTaskThread.joinable())
    {
        mTaskThread.join();
    }
}

void CommandProcessor::handleDeviceLost(Renderer *renderer)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "CommandProcessor::handleDeviceLost");
    // Take mTaskEnqueueMutex lock so that no one is able to add more work to the queue while we
    // drain it and handle device lost.
    std::lock_guard<std::mutex> enqueueLock(mTaskEnqueueMutex);
    (void)waitForAllWorkToBeSubmitted(this);
    // Worker thread is idle and command queue is empty so good to continue
    mCommandQueue->handleDeviceLost(renderer);
}

VkResult CommandProcessor::present(egl::ContextPriority priority,
                                   const VkPresentInfoKHR &presentInfo,
                                   SwapchainStatus *swapchainStatus)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "vkQueuePresentKHR");
    // Verify that we are presenting one and only one swapchain
    ASSERT(presentInfo.swapchainCount == 1);
    ASSERT(presentInfo.pResults == nullptr);

    mCommandQueue->queuePresent(priority, presentInfo, swapchainStatus);
    const VkResult result = swapchainStatus->lastPresentResult;

    // Always make sure update isPending after status has been updated.
    // Can't access swapchainStatus after this assignment because it is marked as no longer pending,
    // and so may get deleted or clobbered by another thread.
    ASSERT(swapchainStatus->isPending);
    swapchainStatus->isPending = false;

    return result;
}

angle::Result CommandProcessor::enqueueSubmitCommands(Context *context,
                                                      ProtectionType protectionType,
                                                      egl::ContextPriority priority,
                                                      VkSemaphore signalSemaphore,
                                                      SharedExternalFence &&externalFence,
                                                      const QueueSerial &submitQueueSerial)
{
    ANGLE_TRY(checkAndPopPendingError(context));

    CommandProcessorTask task;
    task.initFlushAndQueueSubmit(signalSemaphore, std::move(externalFence), protectionType,
                                 priority, submitQueueSerial);

    ANGLE_TRY(queueCommand(std::move(task)));

    mLastEnqueuedSerials.setQueueSerial(submitQueueSerial);

    return angle::Result::Continue;
}

angle::Result CommandProcessor::enqueueSubmitOneOffCommands(
    Context *context,
    ProtectionType protectionType,
    egl::ContextPriority contextPriority,
    VkCommandBuffer commandBufferHandle,
    VkSemaphore waitSemaphore,
    VkPipelineStageFlags waitSemaphoreStageMask,
    SubmitPolicy submitPolicy,
    const QueueSerial &submitQueueSerial)
{
    ANGLE_TRY(checkAndPopPendingError(context));

    CommandProcessorTask task;
    task.initOneOffQueueSubmit(commandBufferHandle, protectionType, contextPriority, waitSemaphore,
                               waitSemaphoreStageMask, submitQueueSerial);
    ANGLE_TRY(queueCommand(std::move(task)));

    mLastEnqueuedSerials.setQueueSerial(submitQueueSerial);

    if (submitPolicy == SubmitPolicy::EnsureSubmitted)
    {
        // Caller has synchronization requirement to have work in GPU pipe when returning from this
        // function.
        ANGLE_TRY(waitForQueueSerialToBeSubmitted(context, submitQueueSerial));
    }

    return angle::Result::Continue;
}

void CommandProcessor::enqueuePresent(egl::ContextPriority contextPriority,
                                      const VkPresentInfoKHR &presentInfo,
                                      SwapchainStatus *swapchainStatus)
{
    ASSERT(!swapchainStatus->isPending);
    swapchainStatus->isPending = true;
    // Always return with VK_SUCCESS initially. When we call acquireNextImage we'll check the
    // return code again. This allows the app to continue working until we really need to know
    // the return code from present.
    swapchainStatus->lastPresentResult = VK_SUCCESS;

    CommandProcessorTask task;
    task.initPresent(contextPriority, presentInfo, swapchainStatus);
    (void)queueCommand(std::move(task));
}

angle::Result CommandProcessor::enqueueFlushWaitSemaphores(
    ProtectionType protectionType,
    egl::ContextPriority priority,
    std::vector<VkSemaphore> &&waitSemaphores,
    std::vector<VkPipelineStageFlags> &&waitSemaphoreStageMasks)
{
    CommandProcessorTask task;
    task.initFlushWaitSemaphores(protectionType, priority, std::move(waitSemaphores),
                                 std::move(waitSemaphoreStageMasks));
    ANGLE_TRY(queueCommand(std::move(task)));

    return angle::Result::Continue;
}

angle::Result CommandProcessor::enqueueFlushOutsideRPCommands(
    Context *context,
    ProtectionType protectionType,
    egl::ContextPriority priority,
    OutsideRenderPassCommandBufferHelper **outsideRPCommands)
{
    ANGLE_TRY(checkAndPopPendingError(context));

    (*outsideRPCommands)->markClosed();

    SecondaryCommandPool *commandPool = nullptr;
    ANGLE_TRY((*outsideRPCommands)->detachCommandPool(context, &commandPool));

    // Detach functions are only used for ring buffer allocators.
    SecondaryCommandMemoryAllocator *allocator = (*outsideRPCommands)->detachAllocator();

    CommandProcessorTask task;
    task.initOutsideRenderPassProcessCommands(protectionType, priority, *outsideRPCommands);
    ANGLE_TRY(queueCommand(std::move(task)));

    ANGLE_TRY(mRenderer->getOutsideRenderPassCommandBufferHelper(context, commandPool, allocator,
                                                                 outsideRPCommands));

    return angle::Result::Continue;
}

angle::Result CommandProcessor::enqueueFlushRenderPassCommands(
    Context *context,
    ProtectionType protectionType,
    egl::ContextPriority priority,
    const RenderPass &renderPass,
    VkFramebuffer framebufferOverride,
    RenderPassCommandBufferHelper **renderPassCommands)
{
    ANGLE_TRY(checkAndPopPendingError(context));

    (*renderPassCommands)->markClosed();

    SecondaryCommandPool *commandPool = nullptr;
    (*renderPassCommands)->detachCommandPool(&commandPool);

    // Detach functions are only used for ring buffer allocators.
    SecondaryCommandMemoryAllocator *allocator = (*renderPassCommands)->detachAllocator();

    CommandProcessorTask task;
    task.initRenderPassProcessCommands(protectionType, priority, *renderPassCommands, &renderPass,
                                       framebufferOverride);
    ANGLE_TRY(queueCommand(std::move(task)));

    ANGLE_TRY(mRenderer->getRenderPassCommandBufferHelper(context, commandPool, allocator,
                                                          renderPassCommands));

    return angle::Result::Continue;
}

angle::Result CommandProcessor::waitForResourceUseToBeSubmitted(Context *context,
                                                                const ResourceUse &use)
{
    if (mCommandQueue->hasResourceUseSubmitted(use))
    {
        ANGLE_TRY(checkAndPopPendingError(context));
    }
    else
    {
        // We do not hold mTaskEnqueueMutex lock, so that we still allow other context to enqueue
        // work while we are processing them.
        std::lock_guard<angle::SimpleMutex> dequeueLock(mTaskDequeueMutex);

        // Do this inside the mutex to prevent new errors adding to the list.
        ANGLE_TRY(checkAndPopPendingError(context));

        size_t maxTaskCount = mTaskQueue.size();
        size_t taskCount    = 0;
        while (taskCount < maxTaskCount && !mCommandQueue->hasResourceUseSubmitted(use))
        {
            CommandProcessorTask task(std::move(mTaskQueue.front()));
            mTaskQueue.pop();
            ANGLE_TRY(processTask(&task));
            taskCount++;
        }
    }
    return angle::Result::Continue;
}

angle::Result CommandProcessor::waitForPresentToBeSubmitted(SwapchainStatus *swapchainStatus)
{
    if (!swapchainStatus->isPending)
    {
        return angle::Result::Continue;
    }

    std::lock_guard<angle::SimpleMutex> dequeueLock(mTaskDequeueMutex);
    size_t maxTaskCount = mTaskQueue.size();
    size_t taskCount    = 0;
    while (taskCount < maxTaskCount && swapchainStatus->isPending)
    {
        CommandProcessorTask task(std::move(mTaskQueue.front()));
        mTaskQueue.pop();
        ANGLE_TRY(processTask(&task));
        taskCount++;
    }
    ASSERT(!swapchainStatus->isPending);
    return angle::Result::Continue;
}

CommandPoolAccess::CommandPoolAccess()  = default;
CommandPoolAccess::~CommandPoolAccess() = default;

// CommandPoolAccess public API implementation. These must be thread safe and never called from
// CommandPoolAccess class itself.
angle::Result CommandPoolAccess::initCommandPool(Context *context,
                                                 ProtectionType protectionType,
                                                 const uint32_t queueFamilyIndex)
{
    std::lock_guard<angle::SimpleMutex> lock(mCmdPoolMutex);
    PersistentCommandPool &commandPool = mPrimaryCommandPoolMap[protectionType];
    return commandPool.init(context, protectionType, queueFamilyIndex);
}

void CommandPoolAccess::destroy(VkDevice device)
{
    std::lock_guard<angle::SimpleMutex> lock(mCmdPoolMutex);
    for (auto &protectionMap : mCommandsStateMap)
    {
        for (CommandsState &state : protectionMap)
        {
            state.waitSemaphores.clear();
            state.waitSemaphoreStageMasks.clear();
            state.primaryCommands.destroy(device);
            state.secondaryCommands.releaseCommandBuffers();
        }
    }

    for (PersistentCommandPool &commandPool : mPrimaryCommandPoolMap)
    {
        commandPool.destroy(device);
    }
}

void CommandPoolAccess::destroyPrimaryCommandBuffer(VkDevice device,
                                                    PrimaryCommandBuffer *primaryCommands) const
{
    ASSERT(primaryCommands->valid());
    std::lock_guard<angle::SimpleMutex> lock(mCmdPoolMutex);

    primaryCommands->destroy(device);
}

angle::Result CommandPoolAccess::collectPrimaryCommandBuffer(Context *context,
                                                             const ProtectionType protectionType,
                                                             PrimaryCommandBuffer *primaryCommands)
{
    ASSERT(primaryCommands->valid());
    std::lock_guard<angle::SimpleMutex> lock(mCmdPoolMutex);

    PersistentCommandPool &commandPool = mPrimaryCommandPoolMap[protectionType];
    ANGLE_TRY(commandPool.collect(context, std::move(*primaryCommands)));

    return angle::Result::Continue;
}

angle::Result CommandPoolAccess::flushOutsideRPCommands(
    Context *context,
    ProtectionType protectionType,
    egl::ContextPriority priority,
    OutsideRenderPassCommandBufferHelper **outsideRPCommands)
{
    std::lock_guard<angle::SimpleMutex> lock(mCmdPoolMutex);
    ANGLE_TRY(ensurePrimaryCommandBufferValidLocked(context, protectionType, priority));
    CommandsState &state = mCommandsStateMap[priority][protectionType];
    return (*outsideRPCommands)->flushToPrimary(context, &state);
}

angle::Result CommandPoolAccess::flushRenderPassCommands(
    Context *context,
    const ProtectionType &protectionType,
    const egl::ContextPriority &priority,
    const RenderPass &renderPass,
    VkFramebuffer framebufferOverride,
    RenderPassCommandBufferHelper **renderPassCommands)
{
    std::lock_guard<angle::SimpleMutex> lock(mCmdPoolMutex);
    ANGLE_TRY(ensurePrimaryCommandBufferValidLocked(context, protectionType, priority));
    CommandsState &state = mCommandsStateMap[priority][protectionType];
    return (*renderPassCommands)->flushToPrimary(context, &state, renderPass, framebufferOverride);
}

void CommandPoolAccess::flushWaitSemaphores(
    ProtectionType protectionType,
    egl::ContextPriority priority,
    std::vector<VkSemaphore> &&waitSemaphores,
    std::vector<VkPipelineStageFlags> &&waitSemaphoreStageMasks)
{
    ASSERT(!waitSemaphores.empty());
    ASSERT(waitSemaphores.size() == waitSemaphoreStageMasks.size());
    std::lock_guard<angle::SimpleMutex> lock(mCmdPoolMutex);

    CommandsState &state = mCommandsStateMap[priority][protectionType];

    state.waitSemaphores.insert(state.waitSemaphores.end(), waitSemaphores.begin(),
                                waitSemaphores.end());
    state.waitSemaphoreStageMasks.insert(state.waitSemaphoreStageMasks.end(),
                                         waitSemaphoreStageMasks.begin(),
                                         waitSemaphoreStageMasks.end());

    waitSemaphores.clear();
    waitSemaphoreStageMasks.clear();
}

angle::Result CommandPoolAccess::getCommandsAndWaitSemaphores(
    Context *context,
    ProtectionType protectionType,
    egl::ContextPriority priority,
    CommandBatch *batchOut,
    std::vector<VkSemaphore> *waitSemaphoresOut,
    std::vector<VkPipelineStageFlags> *waitSemaphoreStageMasksOut)
{
    std::lock_guard<angle::SimpleMutex> lock(mCmdPoolMutex);

    CommandsState &state = mCommandsStateMap[priority][protectionType];
    ASSERT(state.primaryCommands.valid() || state.secondaryCommands.empty());

    // Store the primary CommandBuffer and the reference to CommandPoolAccess in the in-flight list.
    if (state.primaryCommands.valid())
    {
        ANGLE_VK_TRY(context, state.primaryCommands.end());
    }
    batchOut->setPrimaryCommands(std::move(state.primaryCommands), this);

    // Store secondary Command Buffers.
    batchOut->setSecondaryCommands(std::move(state.secondaryCommands));

    // Store wait semaphores.
    *waitSemaphoresOut          = std::move(state.waitSemaphores);
    *waitSemaphoreStageMasksOut = std::move(state.waitSemaphoreStageMasks);

    return angle::Result::Continue;
}

// CommandQueue public API implementation. These must be thread safe and never called from
// CommandQueue class itself.
CommandQueue::CommandQueue()
    : mInFlightCommands(kInFlightCommandsLimit),
      mFinishedCommandBatches(kMaxFinishedCommandsLimit),
      mNumAllCommands(0),
      mPerfCounters{}
{}

CommandQueue::~CommandQueue() = default;

void CommandQueue::destroy(Context *context)
{
    std::lock_guard<angle::SimpleMutex> queueSubmitLock(mQueueSubmitMutex);
    std::lock_guard<angle::SimpleMutex> cmdCompleteLock(mCmdCompleteMutex);
    std::lock_guard<angle::SimpleMutex> cmdReleaseLock(mCmdReleaseMutex);

    mQueueMap.destroy();

    // Assigns an infinite "last completed" serial to force garbage to delete.
    mLastCompletedSerials.fill(Serial::Infinite());

    mCommandPoolAccess.destroy(context->getDevice());

    mFenceRecycler.destroy(context);

    ASSERT(mInFlightCommands.empty());
    ASSERT(mFinishedCommandBatches.empty());
    ASSERT(mNumAllCommands == 0);
}

angle::Result CommandQueue::init(Context *context,
                                 const QueueFamily &queueFamily,
                                 bool enableProtectedContent,
                                 uint32_t queueCount)
{
    std::lock_guard<angle::SimpleMutex> queueSubmitLock(mQueueSubmitMutex);
    std::lock_guard<angle::SimpleMutex> cmdCompleteLock(mCmdCompleteMutex);
    std::lock_guard<angle::SimpleMutex> cmdReleaseLock(mCmdReleaseMutex);

    // In case Renderer gets re-initialized, we can't rely on constructor to do initialization.
    mLastSubmittedSerials.fill(kZeroSerial);
    mLastCompletedSerials.fill(kZeroSerial);

    // Assign before initializing the command pools in order to get the queue family index.
    mQueueMap.initialize(context->getDevice(), queueFamily, enableProtectedContent, 0, queueCount);
    ANGLE_TRY(mCommandPoolAccess.initCommandPool(context, ProtectionType::Unprotected,
                                                 mQueueMap.getQueueFamilyIndex()));

    if (mQueueMap.isProtected())
    {
        ANGLE_TRY(mCommandPoolAccess.initCommandPool(context, ProtectionType::Protected,
                                                     mQueueMap.getQueueFamilyIndex()));
    }
    return angle::Result::Continue;
}

void CommandQueue::handleDeviceLost(Renderer *renderer)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "CommandQueue::handleDeviceLost");
    VkDevice device = renderer->getDevice();
    // Hold all locks while clean up mInFlightCommands.
    std::lock_guard<angle::SimpleMutex> queueSubmitLock(mQueueSubmitMutex);
    std::lock_guard<angle::SimpleMutex> cmdCompleteLock(mCmdCompleteMutex);
    std::lock_guard<angle::SimpleMutex> cmdReleaseLock(mCmdReleaseMutex);

    while (!mInFlightCommands.empty())
    {
        CommandBatch &batch = mInFlightCommands.front();
        // On device loss we need to wait for fence to be signaled before destroying it
        if (batch.hasFence())
        {
            VkResult status = batch.waitFence(device, renderer->getMaxFenceWaitTimeNs());
            // If the wait times out, it is probably not possible to recover from lost device
            ASSERT(status == VK_SUCCESS || status == VK_ERROR_DEVICE_LOST);
        }
        mLastCompletedSerials.setQueueSerial(batch.getQueueSerial());
        batch.destroy(device);
        popInFlightBatchLocked();
    }
}

angle::Result CommandQueue::postSubmitCheck(Context *context)
{
    Renderer *renderer = context->getRenderer();

    // Update mLastCompletedQueueSerial immediately in case any command has been finished.
    ANGLE_TRY(checkAndCleanupCompletedCommands(context));

    VkDeviceSize suballocationGarbageSize = renderer->getSuballocationGarbageSize();
    while (suballocationGarbageSize > kMaxBufferSuballocationGarbageSize)
    {
        // CPU should be throttled to avoid accumulating too much memory garbage waiting to be
        // destroyed. This is important to keep peak memory usage at check when game launched and a
        // lot of staging buffers used for textures upload and then gets released. But if there is
        // only one command buffer in flight, we do not wait here to ensure we keep GPU busy.
        constexpr size_t kMinInFlightBatchesToKeep = 1;
        bool anyGarbageCleaned                     = false;
        ANGLE_TRY(cleanupSomeGarbage(context, kMinInFlightBatchesToKeep, &anyGarbageCleaned));
        if (!anyGarbageCleaned)
        {
            break;
        }
        suballocationGarbageSize = renderer->getSuballocationGarbageSize();
    }

    if (kOutputVmaStatsString)
    {
        renderer->outputVmaStatString();
    }

    return angle::Result::Continue;
}

angle::Result CommandQueue::finishResourceUse(Context *context,
                                              const ResourceUse &use,
                                              uint64_t timeout)
{
    VkDevice device = context->getDevice();
    {
        std::unique_lock<angle::SimpleMutex> lock(mCmdCompleteMutex);
        while (!mInFlightCommands.empty() && !hasResourceUseFinished(use))
        {
            bool finished;
            ANGLE_TRY(checkOneCommandBatchLocked(context, &finished));
            if (!finished)
            {
                ANGLE_VK_TRY(context,
                             mInFlightCommands.front().waitFenceUnlocked(device, timeout, &lock));
            }
        }
        // Check the rest of the commands in case they are also finished.
        ANGLE_TRY(checkCompletedCommandsLocked(context));
    }
    ASSERT(hasResourceUseFinished(use));

    if (!mFinishedCommandBatches.empty())
    {
        ANGLE_TRY(releaseFinishedCommandsAndCleanupGarbage(context));
    }

    return angle::Result::Continue;
}

angle::Result CommandQueue::finishQueueSerial(Context *context,
                                              const QueueSerial &queueSerial,
                                              uint64_t timeout)
{
    ResourceUse use(queueSerial);
    return finishResourceUse(context, use, timeout);
}

angle::Result CommandQueue::waitIdle(Context *context, uint64_t timeout)
{
    // Fill the local variable with lock
    ResourceUse use;
    {
        std::lock_guard<angle::SimpleMutex> lock(mQueueSubmitMutex);
        if (mInFlightCommands.empty())
        {
            return angle::Result::Continue;
        }
        use.setQueueSerial(mInFlightCommands.back().getQueueSerial());
    }

    return finishResourceUse(context, use, timeout);
}

angle::Result CommandQueue::waitForResourceUseToFinishWithUserTimeout(Context *context,
                                                                      const ResourceUse &use,
                                                                      uint64_t timeout,
                                                                      VkResult *result)
{
    // Serial is not yet submitted. This is undefined behaviour, so we can do anything.
    if (!hasResourceUseSubmitted(use))
    {
        WARN() << "Waiting on an unsubmitted serial.";
        *result = VK_TIMEOUT;
        return angle::Result::Continue;
    }

    VkDevice device      = context->getDevice();
    size_t finishedCount = 0;
    {
        std::unique_lock<angle::SimpleMutex> lock(mCmdCompleteMutex);
        *result = hasResourceUseFinished(use) ? VK_SUCCESS : VK_NOT_READY;
        while (!mInFlightCommands.empty() && !hasResourceUseFinished(use))
        {
            bool finished;
            ANGLE_TRY(checkOneCommandBatchLocked(context, &finished));
            if (!finished)
            {
                *result = mInFlightCommands.front().waitFenceUnlocked(device, timeout, &lock);
                // Don't trigger an error on timeout.
                if (*result == VK_TIMEOUT)
                {
                    break;
                }
                else
                {
                    ANGLE_VK_TRY(context, *result);
                }
            }
            else
            {
                *result = hasResourceUseFinished(use) ? VK_SUCCESS : VK_NOT_READY;
            }
        }
        // Do one more check in case more commands also finished.
        ANGLE_TRY(checkCompletedCommandsLocked(context));
        finishedCount = mFinishedCommandBatches.size();
    }

    if (finishedCount > 0)
    {
        ANGLE_TRY(releaseFinishedCommandsAndCleanupGarbage(context));
    }

    return angle::Result::Continue;
}

bool CommandQueue::isBusy(Renderer *renderer) const
{
    // No lock is needed here since we are accessing atomic variables only.
    size_t maxIndex = renderer->getLargestQueueSerialIndexEverAllocated();
    for (SerialIndex i = 0; i <= maxIndex; ++i)
    {
        if (mLastSubmittedSerials[i] > mLastCompletedSerials[i])
        {
            return true;
        }
    }
    return false;
}

angle::Result CommandQueue::submitCommands(Context *context,
                                           ProtectionType protectionType,
                                           egl::ContextPriority priority,
                                           VkSemaphore signalSemaphore,
                                           SharedExternalFence &&externalFence,
                                           const QueueSerial &submitQueueSerial)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "CommandQueue::submitCommands");
    std::lock_guard<angle::SimpleMutex> lock(mQueueSubmitMutex);
    Renderer *renderer = context->getRenderer();
    VkDevice device    = renderer->getDevice();

    ++mPerfCounters.commandQueueSubmitCallsTotal;
    ++mPerfCounters.commandQueueSubmitCallsPerFrame;

    DeviceScoped<CommandBatch> scopedBatch(device);
    CommandBatch &batch = scopedBatch.get();

    batch.setQueueSerial(submitQueueSerial);
    batch.setProtectionType(protectionType);

    std::vector<VkSemaphore> waitSemaphores;
    std::vector<VkPipelineStageFlags> waitSemaphoreStageMasks;

    ANGLE_TRY(mCommandPoolAccess.getCommandsAndWaitSemaphores(
        context, protectionType, priority, &batch, &waitSemaphores, &waitSemaphoreStageMasks));

    mPerfCounters.commandQueueWaitSemaphoresTotal += waitSemaphores.size();

    // Don't make a submission if there is nothing to submit.
    const bool needsQueueSubmit = batch.getPrimaryCommands().valid() ||
                                  signalSemaphore != VK_NULL_HANDLE || externalFence ||
                                  !waitSemaphores.empty();
    VkSubmitInfo submitInfo                   = {};
    VkProtectedSubmitInfo protectedSubmitInfo = {};

    if (needsQueueSubmit)
    {
        InitializeSubmitInfo(&submitInfo, batch.getPrimaryCommands(), waitSemaphores,
                             waitSemaphoreStageMasks, signalSemaphore);

        // No need protected submission if no commands to submit.
        if (protectionType == ProtectionType::Protected && batch.getPrimaryCommands().valid())
        {
            protectedSubmitInfo.sType           = VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO;
            protectedSubmitInfo.pNext           = nullptr;
            protectedSubmitInfo.protectedSubmit = true;
            submitInfo.pNext                    = &protectedSubmitInfo;
        }

        if (!externalFence)
        {
            ANGLE_VK_TRY(context, batch.initFence(context->getDevice(), &mFenceRecycler));
        }
        else
        {
            batch.setExternalFence(std::move(externalFence));
        }

        ++mPerfCounters.vkQueueSubmitCallsTotal;
        ++mPerfCounters.vkQueueSubmitCallsPerFrame;
    }

    return queueSubmitLocked(context, priority, submitInfo, scopedBatch, submitQueueSerial);
}

angle::Result CommandQueue::queueSubmitOneOff(Context *context,
                                              ProtectionType protectionType,
                                              egl::ContextPriority contextPriority,
                                              VkCommandBuffer commandBufferHandle,
                                              VkSemaphore waitSemaphore,
                                              VkPipelineStageFlags waitSemaphoreStageMask,
                                              SubmitPolicy submitPolicy,
                                              const QueueSerial &submitQueueSerial)
{
    std::unique_lock<angle::SimpleMutex> lock(mQueueSubmitMutex);
    DeviceScoped<CommandBatch> scopedBatch(context->getDevice());
    CommandBatch &batch  = scopedBatch.get();
    batch.setQueueSerial(submitQueueSerial);
    batch.setProtectionType(protectionType);

    ANGLE_VK_TRY(context, batch.initFence(context->getDevice(), &mFenceRecycler));

    VkSubmitInfo submitInfo = {};
    submitInfo.sType        = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkProtectedSubmitInfo protectedSubmitInfo = {};
    ASSERT(protectionType == ProtectionType::Unprotected ||
           protectionType == ProtectionType::Protected);
    if (protectionType == ProtectionType::Protected)
    {
        protectedSubmitInfo.sType           = VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO;
        protectedSubmitInfo.pNext           = nullptr;
        protectedSubmitInfo.protectedSubmit = true;
        submitInfo.pNext                    = &protectedSubmitInfo;
    }

    if (commandBufferHandle != VK_NULL_HANDLE)
    {
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &commandBufferHandle;
    }

    if (waitSemaphore != VK_NULL_HANDLE)
    {
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores    = &waitSemaphore;
        submitInfo.pWaitDstStageMask  = &waitSemaphoreStageMask;
    }

    ++mPerfCounters.vkQueueSubmitCallsTotal;
    ++mPerfCounters.vkQueueSubmitCallsPerFrame;

    return queueSubmitLocked(context, contextPriority, submitInfo, scopedBatch, submitQueueSerial);
}

angle::Result CommandQueue::queueSubmitLocked(Context *context,
                                              egl::ContextPriority contextPriority,
                                              const VkSubmitInfo &submitInfo,
                                              DeviceScoped<CommandBatch> &commandBatch,
                                              const QueueSerial &submitQueueSerial)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "CommandQueue::queueSubmitLocked");
    Renderer *renderer = context->getRenderer();

    // CPU should be throttled to avoid mInFlightCommands from growing too fast. Important for
    // off-screen scenarios.
    if (mInFlightCommands.full())
    {
        std::lock_guard<angle::SimpleMutex> lock(mCmdCompleteMutex);
        // Check once more inside the lock in case other thread already finished some/all commands.
        if (mInFlightCommands.full())
        {
            ANGLE_TRY(finishOneCommandBatchLocked(context, renderer->getMaxFenceWaitTimeNs()));
        }
    }
    // Assert will succeed since new batch is pushed only in this method below.
    ASSERT(!mInFlightCommands.full());

    // Also ensure that all mInFlightCommands may be moved into the mFinishedCommandBatches without
    // need of the releaseFinishedCommandsLocked() call.
    ASSERT(mNumAllCommands <= mFinishedCommandBatches.capacity());
    if (mNumAllCommands == mFinishedCommandBatches.capacity())
    {
        std::lock_guard<angle::SimpleMutex> lock(mCmdReleaseMutex);
        ANGLE_TRY(releaseFinishedCommandsLocked(context));
    }
    // Assert will succeed since mNumAllCommands is incremented only in this method below.
    ASSERT(mNumAllCommands < mFinishedCommandBatches.capacity());

    if (submitInfo.sType == VK_STRUCTURE_TYPE_SUBMIT_INFO)
    {
        CommandBatch &batch = commandBatch.get();

        VkQueue queue = getQueue(contextPriority);
        VkFence fence = batch.getFenceHandle();
        ASSERT(fence != VK_NULL_HANDLE);
        ANGLE_VK_TRY(context, vkQueueSubmit(queue, 1, &submitInfo, fence));

        if (batch.getExternalFence())
        {
            // exportFd is exporting VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT_KHR type handle which
            // obeys copy semantics. This means that the fence must already be signaled or the work
            // to signal it is in the graphics pipeline at the time we export the fd.
            // In other words, must call exportFd() after successful vkQueueSubmit() call.
            ExternalFence &externalFence       = *batch.getExternalFence();
            VkFenceGetFdInfoKHR fenceGetFdInfo = {};
            fenceGetFdInfo.sType               = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
            fenceGetFdInfo.fence               = externalFence.getHandle();
            fenceGetFdInfo.handleType          = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT_KHR;
            externalFence.exportFd(renderer->getDevice(), fenceGetFdInfo);
        }
    }

    pushInFlightBatchLocked(commandBatch.release());

    // This must set last so that when this submission appears submitted, it actually already
    // submitted and enqueued to mInFlightCommands.
    mLastSubmittedSerials.setQueueSerial(submitQueueSerial);
    return angle::Result::Continue;
}

void CommandQueue::queuePresent(egl::ContextPriority contextPriority,
                                const VkPresentInfoKHR &presentInfo,
                                SwapchainStatus *swapchainStatus)
{
    std::lock_guard<angle::SimpleMutex> lock(mQueueSubmitMutex);
    VkQueue queue                      = getQueue(contextPriority);
    swapchainStatus->lastPresentResult = vkQueuePresentKHR(queue, &presentInfo);
}

const angle::VulkanPerfCounters CommandQueue::getPerfCounters() const
{
    std::lock_guard<angle::SimpleMutex> lock(mQueueSubmitMutex);
    return mPerfCounters;
}

void CommandQueue::resetPerFramePerfCounters()
{
    std::lock_guard<angle::SimpleMutex> lock(mQueueSubmitMutex);
    mPerfCounters.commandQueueSubmitCallsPerFrame = 0;
    mPerfCounters.vkQueueSubmitCallsPerFrame      = 0;
}

angle::Result CommandQueue::releaseFinishedCommandsAndCleanupGarbage(Context *context)
{
    Renderer *renderer = context->getRenderer();
    if (renderer->isAsyncCommandBufferResetAndGarbageCleanupEnabled())
    {
        renderer->requestAsyncCommandsAndGarbageCleanup(context);
    }
    else
    {
        // Do immediate command buffer reset and garbage cleanup
        ANGLE_TRY(releaseFinishedCommands(context));
        renderer->cleanupGarbage(nullptr);
    }

    return angle::Result::Continue;
}

angle::Result CommandQueue::cleanupSomeGarbage(Context *context,
                                               size_t minInFlightBatchesToKeep,
                                               bool *anyGarbageCleanedOut)
{
    Renderer *renderer = context->getRenderer();

    bool anyGarbageCleaned = false;

    renderer->cleanupGarbage(&anyGarbageCleaned);

    while (!anyGarbageCleaned)
    {
        {
            std::lock_guard<angle::SimpleMutex> lock(mCmdCompleteMutex);
            if (mInFlightCommands.size() <= minInFlightBatchesToKeep)
            {
                break;
            }
            ANGLE_TRY(finishOneCommandBatchLocked(context, renderer->getMaxFenceWaitTimeNs()));
        }
        renderer->cleanupGarbage(&anyGarbageCleaned);
    }

    if (anyGarbageCleanedOut != nullptr)
    {
        *anyGarbageCleanedOut = anyGarbageCleaned;
    }

    return angle::Result::Continue;
}

// CommandQueue private API implementation. These are called by public API, so lock already held.
angle::Result CommandQueue::checkOneCommandBatchLocked(Context *context, bool *finished)
{
    ASSERT(!mInFlightCommands.empty());

    CommandBatch &batch = mInFlightCommands.front();
    *finished           = false;
    if (batch.hasFence())
    {
        VkResult status = batch.getFenceStatus(context->getDevice());
        if (status == VK_NOT_READY)
        {
            return angle::Result::Continue;
        }
        ANGLE_VK_TRY(context, status);
    }

    onCommandBatchFinishedLocked(std::move(batch));
    *finished = true;

    return angle::Result::Continue;
}

angle::Result CommandQueue::finishOneCommandBatchLocked(Context *context, uint64_t timeout)
{
    ASSERT(!mInFlightCommands.empty());

    CommandBatch &batch = mInFlightCommands.front();
    if (batch.hasFence())
    {
        VkResult status = batch.waitFence(context->getDevice(), timeout);
        ANGLE_VK_TRY(context, status);
    }

    onCommandBatchFinishedLocked(std::move(batch));

    return angle::Result::Continue;
}

void CommandQueue::onCommandBatchFinishedLocked(CommandBatch &&batch)
{
    // Finished.
    mLastCompletedSerials.setQueueSerial(batch.getQueueSerial());

    // Move command batch to mFinishedCommandBatches.
    moveInFlightBatchToFinishedQueueLocked(std::move(batch));
}

angle::Result CommandQueue::releaseFinishedCommandsLocked(Context *context)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "releaseFinishedCommandsLocked");

    while (!mFinishedCommandBatches.empty())
    {
        CommandBatch &batch = mFinishedCommandBatches.front();
        ASSERT(batch.getQueueSerial() <= mLastCompletedSerials);
        ANGLE_TRY(batch.release(context));
        popFinishedBatchLocked();
    }

    return angle::Result::Continue;
}

angle::Result CommandQueue::checkCompletedCommandsLocked(Context *context)
{
    while (!mInFlightCommands.empty())
    {
        bool finished;
        ANGLE_TRY(checkOneCommandBatchLocked(context, &finished));
        if (!finished)
        {
            break;
        }
    }
    return angle::Result::Continue;
}

void CommandQueue::pushInFlightBatchLocked(CommandBatch &&batch)
{
    // Need to increment before the push to prevent possible decrement from 0.
    ++mNumAllCommands;
    mInFlightCommands.push(std::move(batch));
}

void CommandQueue::moveInFlightBatchToFinishedQueueLocked(CommandBatch &&batch)
{
    // This must not happen, since we always leave space in the queue during queueSubmitLocked.
    ASSERT(!mFinishedCommandBatches.full());
    ASSERT(&batch == &mInFlightCommands.front());

    mFinishedCommandBatches.push(std::move(batch));
    mInFlightCommands.pop();
    // No mNumAllCommands update since batch was simply moved to the other queue.
}

void CommandQueue::popFinishedBatchLocked()
{
    mFinishedCommandBatches.pop();
    // Need to decrement after the pop to prevent possible push over the limit.
    ASSERT(mNumAllCommands > 0);
    --mNumAllCommands;
}

void CommandQueue::popInFlightBatchLocked()
{
    mInFlightCommands.pop();
    // Need to decrement after the pop to prevent possible push over the limit.
    ASSERT(mNumAllCommands > 0);
    --mNumAllCommands;
}

// QueuePriorities:
constexpr float kVulkanQueuePriorityLow    = 0.0;
constexpr float kVulkanQueuePriorityMedium = 0.4;
constexpr float kVulkanQueuePriorityHigh   = 1.0;

const float QueueFamily::kQueuePriorities[static_cast<uint32_t>(egl::ContextPriority::EnumCount)] =
    {kVulkanQueuePriorityMedium, kVulkanQueuePriorityHigh, kVulkanQueuePriorityLow};

DeviceQueueMap::~DeviceQueueMap() {}

void DeviceQueueMap::destroy()
{
    // Force all commands to finish by flushing all queues.
    for (const QueueAndIndex &queueAndIndex : mQueueAndIndices)
    {
        if (queueAndIndex.queue != VK_NULL_HANDLE)
        {
            vkQueueWaitIdle(queueAndIndex.queue);
        }
    }
}

void DeviceQueueMap::initialize(VkDevice device,
                                const QueueFamily &queueFamily,
                                bool makeProtected,
                                uint32_t queueIndex,
                                uint32_t queueCount)
{
    // QueueIndexing:
    constexpr uint32_t kQueueIndexMedium = 0;
    constexpr uint32_t kQueueIndexHigh   = 1;
    constexpr uint32_t kQueueIndexLow    = 2;

    ASSERT(queueCount);
    ASSERT((queueIndex + queueCount) <= queueFamily.getProperties()->queueCount);
    mQueueFamilyIndex = queueFamily.getQueueFamilyIndex();
    mIsProtected      = makeProtected;

    VkQueue queue = VK_NULL_HANDLE;
    GetDeviceQueue(device, makeProtected, mQueueFamilyIndex, queueIndex + kQueueIndexMedium,
                   &queue);
    mQueueAndIndices[egl::ContextPriority::Medium] = {egl::ContextPriority::Medium, queue,
                                                      queueIndex + kQueueIndexMedium};

    // If at least 2 queues, High has its own queue
    if (queueCount > 1)
    {
        GetDeviceQueue(device, makeProtected, mQueueFamilyIndex, queueIndex + kQueueIndexHigh,
                       &queue);
        mQueueAndIndices[egl::ContextPriority::High] = {egl::ContextPriority::High, queue,
                                                        queueIndex + kQueueIndexHigh};
    }
    else
    {
        mQueueAndIndices[egl::ContextPriority::High] =
            mQueueAndIndices[egl::ContextPriority::Medium];
    }
    // If at least 3 queues, Low has its own queue. Adjust Low priority.
    if (queueCount > 2)
    {
        GetDeviceQueue(device, makeProtected, mQueueFamilyIndex, queueIndex + kQueueIndexLow,
                       &queue);
        mQueueAndIndices[egl::ContextPriority::Low] = {egl::ContextPriority::Low, queue,
                                                       queueIndex + kQueueIndexLow};
    }
    else
    {
        mQueueAndIndices[egl::ContextPriority::Low] =
            mQueueAndIndices[egl::ContextPriority::Medium];
    }
}

void QueueFamily::initialize(const VkQueueFamilyProperties &queueFamilyProperties,
                             uint32_t queueFamilyIndex)
{
    mProperties       = queueFamilyProperties;
    mQueueFamilyIndex = queueFamilyIndex;
}

uint32_t QueueFamily::FindIndex(const std::vector<VkQueueFamilyProperties> &queueFamilyProperties,
                                VkQueueFlags flags,
                                int32_t matchNumber,
                                uint32_t *matchCount)
{
    uint32_t index = QueueFamily::kInvalidIndex;
    uint32_t count = 0;

    for (uint32_t familyIndex = 0; familyIndex < queueFamilyProperties.size(); ++familyIndex)
    {
        const auto &queueInfo = queueFamilyProperties[familyIndex];
        if ((queueInfo.queueFlags & flags) == flags)
        {
            ASSERT(queueInfo.queueCount > 0);
            count++;
            if ((index == QueueFamily::kInvalidIndex) && (matchNumber-- == 0))
            {
                index = familyIndex;
            }
        }
    }
    if (matchCount)
    {
        *matchCount = count;
    }

    return index;
}

}  // namespace vk
}  // namespace rx

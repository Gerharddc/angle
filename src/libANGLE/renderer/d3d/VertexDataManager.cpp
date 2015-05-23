//
// Copyright (c) 2002-2012 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// VertexDataManager.h: Defines the VertexDataManager, a class that
// runs the Buffer translation process.

#include "libANGLE/renderer/d3d/VertexDataManager.h"

#include "libANGLE/Buffer.h"
#include "libANGLE/Program.h"
#include "libANGLE/State.h"
#include "libANGLE/VertexAttribute.h"
#include "libANGLE/VertexArray.h"
#include "libANGLE/renderer/d3d/BufferD3D.h"
#include "libANGLE/renderer/d3d/VertexBuffer.h"

namespace
{
    enum { INITIAL_STREAM_BUFFER_SIZE = 1024*1024 };
    // This has to be at least 4k or else it fails on ATI cards.
    enum { CONSTANT_VERTEX_BUFFER_SIZE = 4096 };
}

namespace rx
{

static int ElementsInBuffer(const gl::VertexAttribute &attrib, unsigned int size)
{
    // Size cannot be larger than a GLsizei
    if (size > static_cast<unsigned int>(std::numeric_limits<int>::max()))
    {
        size = static_cast<unsigned int>(std::numeric_limits<int>::max());
    }

    GLsizei stride = ComputeVertexAttributeStride(attrib);
    return (size - attrib.offset % stride + (stride - ComputeVertexAttributeTypeSize(attrib))) / stride;
}

static int StreamingBufferElementCount(const gl::VertexAttribute &attrib, int vertexDrawCount, int instanceDrawCount)
{
    // For instanced rendering, we draw "instanceDrawCount" sets of "vertexDrawCount" vertices.
    //
    // A vertex attribute with a positive divisor loads one instanced vertex for every set of
    // non-instanced vertices, and the instanced vertex index advances once every "mDivisor" instances.
    if (instanceDrawCount > 0 && attrib.divisor > 0)
    {
        // When instanceDrawCount is not a multiple attrib.divisor, the division must round up.
        // For instance, with 5 non-instanced vertices and a divisor equal to 3, we need 2 instanced vertices.
        return (instanceDrawCount + attrib.divisor - 1) / attrib.divisor;
    }

    return vertexDrawCount;
}

VertexDataManager::VertexDataManager(BufferFactoryD3D *factory)
    : mFactory(factory)
{
    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        mCurrentValue[i].FloatValues[0] = std::numeric_limits<float>::quiet_NaN();
        mCurrentValue[i].FloatValues[1] = std::numeric_limits<float>::quiet_NaN();
        mCurrentValue[i].FloatValues[2] = std::numeric_limits<float>::quiet_NaN();
        mCurrentValue[i].FloatValues[3] = std::numeric_limits<float>::quiet_NaN();
        mCurrentValue[i].Type = GL_FLOAT;
        mCurrentValueBuffer[i] = NULL;
        mCurrentValueOffsets[i] = 0;
    }

    mStreamingBuffer = new StreamingVertexBufferInterface(factory, INITIAL_STREAM_BUFFER_SIZE);

    if (!mStreamingBuffer)
    {
        ERR("Failed to allocate the streaming vertex buffer.");
    }
}

VertexDataManager::~VertexDataManager()
{
    delete mStreamingBuffer;

    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        delete mCurrentValueBuffer[i];
    }
}

void VertexDataManager::hintUnmapAllResources(const std::vector<gl::VertexAttribute> &vertexAttributes)
{
    mStreamingBuffer->getVertexBuffer()->hintUnmapResource();

    for (size_t i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        if (mCurrentValueBuffer[i] != NULL)
        {
            mCurrentValueBuffer[i]->getVertexBuffer()->hintUnmapResource();
        }
    }
}

gl::Error VertexDataManager::prepareVertexData(const gl::State &state, GLint start, GLsizei count,
                                               TranslatedAttribute *translated, GLsizei instances)
{
    if (!mStreamingBuffer)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Internal streaming vertex buffer is unexpectedly NULL.");
    }

    const gl::VertexArray *vertexArray = state.getVertexArray();
    const std::vector<gl::VertexAttribute> &vertexAttributes = vertexArray->getVertexAttributes();

    // Invalidate static buffers that don't contain matching attributes
    for (int attributeIndex = 0; attributeIndex < gl::MAX_VERTEX_ATTRIBS; attributeIndex++)
    {
        translated[attributeIndex].active = (state.getProgram()->getSemanticIndex(attributeIndex) != -1);
        if (translated[attributeIndex].active && vertexAttributes[attributeIndex].enabled)
        {
            prepareStaticBufferForAttribute(vertexAttributes[attributeIndex], state.getVertexAttribCurrentValue(attributeIndex));
        }
    }

    // Reserve the required space in the buffers
    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        if (translated[i].active && vertexAttributes[i].enabled)
        {
            gl::Error error = reserveSpaceForAttrib(vertexAttributes[i], state.getVertexAttribCurrentValue(i), count, instances);
            if (error.isError())
            {
                return error;
            }
        }
    }

    // Perform the vertex data translations
    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        const gl::VertexAttribute &curAttrib = vertexAttributes[i];
        if (translated[i].active)
        {
            if (curAttrib.enabled)
            {
                gl::Error error = storeAttribute(curAttrib, state.getVertexAttribCurrentValue(i),
                                                 &translated[i], start, count, instances);

                if (error.isError())
                {
                    hintUnmapAllResources(vertexAttributes);
                    return error;
                }
            }
            else
            {
                if (!mCurrentValueBuffer[i])
                {
                    mCurrentValueBuffer[i] = new StreamingVertexBufferInterface(mFactory, CONSTANT_VERTEX_BUFFER_SIZE);
                }

                gl::Error error = storeCurrentValue(curAttrib, state.getVertexAttribCurrentValue(i), &translated[i],
                                                    &mCurrentValue[i], &mCurrentValueOffsets[i],
                                                    mCurrentValueBuffer[i]);
                if (error.isError())
                {
                    hintUnmapAllResources(vertexAttributes);
                    return error;
                }
            }
        }
    }

    // Hint to unmap all the resources
    hintUnmapAllResources(vertexAttributes);

    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        const gl::VertexAttribute &curAttrib = vertexAttributes[i];
        if (translated[i].active && curAttrib.enabled)
        {
            gl::Buffer *buffer = curAttrib.buffer.get();

            if (buffer)
            {
                BufferD3D *bufferImpl = GetImplAs<BufferD3D>(buffer);
                bufferImpl->promoteStaticVertexUsageForAttrib(curAttrib, count * ComputeVertexAttributeTypeSize(curAttrib));
            }
        }
    }

    return gl::Error(GL_NO_ERROR);
}

void VertexDataManager::prepareStaticBufferForAttribute(const gl::VertexAttribute &attrib,
                                                        const gl::VertexAttribCurrentValueData &currentValue) const
{
    gl::Buffer *buffer = attrib.buffer.get();

    if (buffer)
    {
        BufferD3D *bufferImpl = GetImplAs<BufferD3D>(buffer);

        // This will create the static buffer in the right circumstances
        StaticVertexBufferInterface *staticBuffer = bufferImpl->getStaticVertexBufferForAttribute(attrib);
        UNUSED_ASSERTION_VARIABLE(staticBuffer);

        // This check validates that a valid static vertex buffer was returned above
        ASSERT(!(staticBuffer &&
                 staticBuffer->getBufferSize() > 0 &&
                 !staticBuffer->lookupAttribute(attrib, NULL) &&
                 !staticBuffer->directStoragePossible(attrib, currentValue)));
    }
}

gl::Error VertexDataManager::reserveSpaceForAttrib(const gl::VertexAttribute &attrib,
                                                   const gl::VertexAttribCurrentValueData &currentValue,
                                                   GLsizei count,
                                                   GLsizei instances) const
{
    gl::Buffer *buffer = attrib.buffer.get();
    BufferD3D *bufferImpl = buffer ? GetImplAs<BufferD3D>(buffer) : NULL;
    StaticVertexBufferInterface *staticBuffer = bufferImpl ? bufferImpl->getStaticVertexBufferForAttribute(attrib) : NULL;
    VertexBufferInterface *vertexBuffer = staticBuffer ? staticBuffer : static_cast<VertexBufferInterface*>(mStreamingBuffer);

    if (!vertexBuffer->directStoragePossible(attrib, currentValue))
    {
        if (staticBuffer)
        {
            if (staticBuffer->getBufferSize() == 0)
            {
                int totalCount = ElementsInBuffer(attrib, bufferImpl->getSize());
                gl::Error error = staticBuffer->reserveVertexSpace(attrib, totalCount, 0);
                if (error.isError())
                {
                    return error;
                }
            }
        }
        else
        {
            int totalCount = StreamingBufferElementCount(attrib, count, instances);
            ASSERT(!bufferImpl || ElementsInBuffer(attrib, bufferImpl->getSize()) >= totalCount);

            gl::Error error = mStreamingBuffer->reserveVertexSpace(attrib, totalCount, instances);
            if (error.isError())
            {
                return error;
            }
        }
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error VertexDataManager::storeAttribute(const gl::VertexAttribute &attrib,
                                            const gl::VertexAttribCurrentValueData &currentValue,
                                            TranslatedAttribute *translated,
                                            GLint start,
                                            GLsizei count,
                                            GLsizei instances)
{
    gl::Buffer *buffer = attrib.buffer.get();
    ASSERT(buffer || attrib.pointer);

    BufferD3D *storage = buffer ? GetImplAs<BufferD3D>(buffer) : NULL;
    StaticVertexBufferInterface *staticBuffer = storage ? storage->getStaticVertexBufferForAttribute(attrib) : NULL;
    VertexBufferInterface *vertexBuffer = staticBuffer ? staticBuffer : static_cast<VertexBufferInterface*>(mStreamingBuffer);
    bool directStorage = vertexBuffer->directStoragePossible(attrib, currentValue);

    unsigned int streamOffset = 0;
    unsigned int outputElementSize = 0;

    // Instanced vertices do not apply the 'start' offset
    GLint firstVertexIndex = (instances > 0 && attrib.divisor > 0 ? 0 : start);

    if (directStorage)
    {
        outputElementSize = ComputeVertexAttributeStride(attrib);
        streamOffset = static_cast<unsigned int>(attrib.offset + outputElementSize * firstVertexIndex);
    }
    else if (staticBuffer)
    {
        gl::Error error = staticBuffer->getVertexBuffer()->getSpaceRequired(attrib, 1, 0, &outputElementSize);
        if (error.isError())
        {
            return error;
        }

        if (!staticBuffer->lookupAttribute(attrib, &streamOffset))
        {
            // Convert the entire buffer
            int totalCount = ElementsInBuffer(attrib, storage->getSize());
            int startIndex = attrib.offset / ComputeVertexAttributeStride(attrib);

            error = staticBuffer->storeVertexAttributes(attrib, currentValue, -startIndex, totalCount,
                                                        0, &streamOffset);

            // Each staticBuffer only contains the data for one attribute, so we know that it won't be modified again.
            // We can therefore safely unmap it here without hurting perf.
            staticBuffer->getVertexBuffer()->hintUnmapResource();

            if (error.isError())
            {
                return error;
            }
        }

        unsigned int firstElementOffset = (attrib.offset / ComputeVertexAttributeStride(attrib)) * outputElementSize;
        unsigned int startOffset = (instances == 0 || attrib.divisor == 0) ? firstVertexIndex * outputElementSize : 0;
        if (streamOffset + firstElementOffset + startOffset < streamOffset)
        {
            return gl::Error(GL_OUT_OF_MEMORY);
        }

        streamOffset += firstElementOffset + startOffset;
    }
    else
    {
        int totalCount = StreamingBufferElementCount(attrib, count, instances);
        gl::Error error = mStreamingBuffer->getVertexBuffer()->getSpaceRequired(attrib, 1, 0, &outputElementSize);
        if (error.isError())
        {
            return error;
        }

        error = mStreamingBuffer->storeVertexAttributes(attrib, currentValue, firstVertexIndex,
                                                        totalCount, instances, &streamOffset);
        if (error.isError())
        {
            return error;
        }
    }

    translated->storage = directStorage ? storage : NULL;
    translated->vertexBuffer = vertexBuffer->getVertexBuffer();
    translated->serial = directStorage ? storage->getSerial() : vertexBuffer->getSerial();
    translated->divisor = attrib.divisor;

    translated->attribute = &attrib;
    translated->currentValueType = currentValue.Type;
    translated->stride = outputElementSize;
    translated->offset = streamOffset;

    return gl::Error(GL_NO_ERROR);
}

gl::Error VertexDataManager::storeCurrentValue(const gl::VertexAttribute &attrib,
                                               const gl::VertexAttribCurrentValueData &currentValue,
                                               TranslatedAttribute *translated,
                                               gl::VertexAttribCurrentValueData *cachedValue,
                                               size_t *cachedOffset,
                                               StreamingVertexBufferInterface *buffer)
{
    if (*cachedValue != currentValue)
    {
        gl::Error error = buffer->reserveVertexSpace(attrib, 1, 0);
        if (error.isError())
        {
            return error;
        }

        unsigned int streamOffset;
        error = buffer->storeVertexAttributes(attrib, currentValue, 0, 1, 0, &streamOffset);
        if (error.isError())
        {
            return error;
        }

        *cachedValue = currentValue;
        *cachedOffset = streamOffset;
    }

    translated->storage = NULL;
    translated->vertexBuffer = buffer->getVertexBuffer();
    translated->serial = buffer->getSerial();
    translated->divisor = 0;

    translated->attribute = &attrib;
    translated->currentValueType = currentValue.Type;
    translated->stride = 0;
    translated->offset = *cachedOffset;

    return gl::Error(GL_NO_ERROR);
}

}

#include "SliceRenderer.hpp"
#include "FAST/Exception.hpp"
#include "FAST/DeviceManager.hpp"
#include "FAST/Utility.hpp"
#include "FAST/Data/Image.hpp"
#include "FAST/SceneGraph.hpp"
#include <boost/thread/lock_guard.hpp>
#if defined(__APPLE__) || defined(__MACOSX)
#include <OpenCL/cl_gl.h>
#include <OpenGL/gl.h>
#include <OpenGL/OpenGL.h>
#else
#if _WIN32
#include <GL/gl.h>
#include <CL/cl_gl.h>
#else
#include <GL/glx.h>
#include <CL/cl_gl.h>
#include <GL/glu.h>
#endif
#endif

using namespace fast;

#ifndef GL_RGBA32F // this is missing on windows and mac for some reason
#define GL_RGBA32F 0x8814
#endif


void SliceRenderer::execute() {
    boost::lock_guard<boost::mutex> lock(mMutex);
    mImageToRender = getStaticInputData<Image>(0);

    if(mImageToRender->getDimensions() != 3)
        throw Exception("The SliceRenderer only supports 3D images");

    // Determine level and window
    float window = mWindow;
    float level = mLevel;
    // If mWindow/mLevel is equal to -1 use default level/window values
    if(window == -1) {
        window = getDefaultIntensityWindow(mImageToRender->getDataType());
    }
    if(level == -1) {
        level = getDefaultIntensityLevel(mImageToRender->getDataType());
    }

    OpenCLDevice::pointer device = getMainDevice();

    // Determine slice nr and width and height of the texture to render to
    unsigned int sliceNr;
    if(mSliceNr == -1) {
        switch(mSlicePlane) {
        case PLANE_X:
            sliceNr = mImageToRender->getWidth()/2;
            break;
        case PLANE_Y:
            sliceNr = mImageToRender->getHeight()/2;
            break;
        case PLANE_Z:
            sliceNr = mImageToRender->getDepth()/2;
            break;
        }
    } else if(mSliceNr >= 0) {
        // Check that mSliceNr is valid
        sliceNr = mSliceNr;
        switch(mSlicePlane) {
        case PLANE_X:
            if(sliceNr >= mImageToRender->getWidth())
                sliceNr = mImageToRender->getWidth()-1;
            break;
        case PLANE_Y:
            if(sliceNr >= mImageToRender->getHeight())
                sliceNr = mImageToRender->getHeight()-1;
            break;
        case PLANE_Z:
            if(sliceNr >= mImageToRender->getDepth())
                sliceNr = mImageToRender->getDepth()-1;
            break;
        }
    } else {
        throw Exception("Slice to render was below 0 in SliceRenderer");
    }
    unsigned int slicePlaneNr;
    switch(mSlicePlane) {
        case PLANE_X:
            slicePlaneNr = 0;
            mWidth = mImageToRender->getHeight();
            mHeight = mImageToRender->getDepth();
            break;
        case PLANE_Y:
            slicePlaneNr = 1;
            mWidth = mImageToRender->getWidth();
            mHeight = mImageToRender->getDepth();
            break;
        case PLANE_Z:
            slicePlaneNr = 2;
            mWidth = mImageToRender->getWidth();
            mHeight = mImageToRender->getHeight();
            break;
    }
    mSliceNr = sliceNr;

    OpenCLImageAccess::pointer access = mImageToRender->getOpenCLImageAccess(ACCESS_READ, device);
    cl::Image3D* clImage = access->get3DImage();

    glEnable(GL_TEXTURE_2D);
    if(mTextureIsCreated) {
        // Delete old texture
        glDeleteTextures(1, &mTexture);
    }

    // Create OpenGL texture
    glGenTextures(1, &mTexture);
    glBindTexture(GL_TEXTURE_2D, mTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, mWidth, mHeight, 0, GL_RGBA, GL_FLOAT, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFinish();

    // Create CL-GL image
#if defined(CL_VERSION_1_2)
    // TODO this sometimes locks. Why???
    mImageGL = cl::ImageGL(
            device->getContext(),
            CL_MEM_READ_WRITE,
            GL_TEXTURE_2D,
            0,
            mTexture
    );
#else
    mImageGL = cl::Image2DGL(
            device->getContext(),
            CL_MEM_READ_WRITE,
            GL_TEXTURE_2D,
            0,
            mTexture
    );
#endif

    // Run kernel to fill the texture
    cl::CommandQueue queue = device->getCommandQueue();
    std::vector<cl::Memory> v;
    v.push_back(mImageGL);
    queue.enqueueAcquireGLObjects(&v);

    recompileOpenCLCode(mImageToRender);
    mKernel.setArg(0, *clImage);
    mKernel.setArg(1, mImageGL);
    mKernel.setArg(2, sliceNr);
    mKernel.setArg(3, level);
    mKernel.setArg(4, window);
    mKernel.setArg(5, slicePlaneNr);
    queue.enqueueNDRangeKernel(
            mKernel,
            cl::NullRange,
            cl::NDRange(mWidth, mHeight),
            cl::NullRange
    );

    queue.enqueueReleaseGLObjects(&v);
    queue.finish();

    mTextureIsCreated = true;
}

void SliceRenderer::setInputConnection(ProcessObjectPort port) {
    releaseInputAfterExecute(0, false);
    ProcessObject::setInputConnection(0, port);
}

void SliceRenderer::recompileOpenCLCode(Image::pointer input) {
    // Check if code has to be recompiled
    bool recompile = false;
    if(!mTextureIsCreated) {
        recompile = true;
    } else {
        if(mTypeCLCodeCompiledFor != input->getDataType())
            recompile = true;
    }
    if(!recompile)
        return;
    std::string buildOptions = "";
    if(input->getDataType() == TYPE_FLOAT) {
        buildOptions = "-DTYPE_FLOAT";
    } else if(input->getDataType() == TYPE_INT8 || input->getDataType() == TYPE_INT16) {
        buildOptions = "-DTYPE_INT";
    } else {
        buildOptions = "-DTYPE_UINT";
    }
    OpenCLDevice::pointer device = getMainDevice();
    mKernel = cl::Kernel(getOpenCLProgram(device, "", buildOptions), "renderToTexture");
    mTypeCLCodeCompiledFor = input->getDataType();
}


SliceRenderer::SliceRenderer() : Renderer() {
    createInputPort<Image>(0, false);
    createOpenCLProgram(std::string(FAST_SOURCE_DIR) + "/Visualization/SliceRenderer/SliceRenderer.cl");
    mTextureIsCreated = false;
    mIsModified = true;
    mSlicePlane = PLANE_Z;
    mSliceNr = -1;
    mScale = 1.0;
}

void SliceRenderer::draw() {
    boost::lock_guard<boost::mutex> lock(mMutex);
    if(!mTextureIsCreated)
        return;

    AffineTransformation::pointer transform = SceneGraph::getAffineTransformationFromData(mImageToRender);
    transform->scale(mImageToRender->getSpacing());
    glMultMatrixf(transform->data());

    glBindTexture(GL_TEXTURE_2D, mTexture);

    // Draw slice in voxel coordinates
    glBegin(GL_QUADS);
    switch(mSlicePlane) {
    case PLANE_Z:
        glTexCoord2i(0, 1);
        glVertex3f(0, mHeight, mSliceNr);
        glTexCoord2i(1, 1);
        glVertex3f(mWidth, mHeight, mSliceNr);
        glTexCoord2i(1, 0);
        glVertex3f( mWidth,0, mSliceNr);
        glTexCoord2i(0, 0);
        glVertex3f(0,0, mSliceNr);
        break;
    case PLANE_Y:
        glTexCoord2i(0, 1);
        glVertex3f(0, mSliceNr, mHeight);
        glTexCoord2i(1, 1);
        glVertex3f(mWidth, mSliceNr, mHeight);
        glTexCoord2i(1, 0);
        glVertex3f( mWidth,mSliceNr, 0 );
        glTexCoord2i(0, 0);
        glVertex3f(0,mSliceNr,0 );
        break;
    case PLANE_X:
        glTexCoord2i(0, 1);
        glVertex3f(mSliceNr, 0 , mHeight);
        glTexCoord2i(1, 1);
        glVertex3f(mSliceNr, mWidth, mHeight);
        glTexCoord2i(1, 0);
        glVertex3f( mSliceNr, mWidth, 0 );
        glTexCoord2i(0, 0);
        glVertex3f(mSliceNr,0,0 );
    break;
    }
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
}

void SliceRenderer::setSliceToRender(unsigned int sliceNr) {
    mSliceNr = sliceNr;
    mIsModified = true;
}

void SliceRenderer::setSlicePlane(PlaneType plane) {
    mSlicePlane = plane;
    mIsModified = true;
}

BoundingBox SliceRenderer::getBoundingBox() {

    BoundingBox inputBoundingBox = mImageToRender->getBoundingBox();
    MatrixXf corners = inputBoundingBox.getCorners();
    // Shrink bounding box so that it covers the slice and not the entire data
    switch(mSlicePlane) {
        case PLANE_X:
            for(uint i = 0; i < 8; i++)
                corners(i,0) = mSliceNr;
            break;
        case PLANE_Y:
            for(uint i = 0; i < 8; i++)
                corners(i,1) = mSliceNr;
            break;
        case PLANE_Z:
            for(uint i = 0; i < 8; i++)
                corners(i,2) = mSliceNr;
            break;
    }
    BoundingBox shrinkedBox(corners);
    AffineTransformation::pointer transform = SceneGraph::getAffineTransformationFromData(mImageToRender);
    transform->scale(mImageToRender->getSpacing());
    BoundingBox transformedBoundingBox = shrinkedBox.getTransformedBoundingBox(transform);
    return transformedBoundingBox;
}


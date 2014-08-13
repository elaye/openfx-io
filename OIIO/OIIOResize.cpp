/*
 OIIOResize plugin.
 Resize images using OIIO.

 Copyright (C) 2014 INRIA
 Author: Frederic Devernay <frederic.devernay@inria.fr>

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

 Neither the name of the {organization} nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 INRIA
 Domaine de Voluceau
 Rocquencourt - B.P. 105
 78153 Le Chesnay Cedex - France

 */

#ifdef DEBUG

#include "OIIOResize.h"


#include "ofxsProcessing.H"
#include "ofxsCopier.h"
#include "ofxsProcessing.h"
#include "../SupportExt/ofxsFormatResolution.h"
#include "../SupportExt/ofxsMerging.h"
#include <OpenImageIO/imageio.h>
/*
 unfortunately, OpenImageIO/imagebuf.h includes OpenImageIO/thread.h,
 which includes boost/thread.hpp,
 which includes boost/system/error_code.hpp,
 which requires the library boost_system to get the symbol boost::system::system_category().
 
 the following define prevents including error_code.hpp, which is not used anyway.
 */
#define OPENIMAGEIO_THREAD_H
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/filter.h>

#define kPluginName "ResizeOIIO"
#define kPluginGrouping "Transform"
#define kPluginDescription  "Use OpenImageIO to resize images."

#define kPluginIdentifier "fr.inria.openfx:OIIOResize"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 0
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kRenderThreadSafety eRenderFullySafe

#define kTypeParamName "type"
#define kTypeParamLabel "Type"
#define kTypeParamHint "Format: Converts between formats, the image is resized to fit in the target format. " \
"Size: Scales to fit into a box of a given width and height. " \
"Scale: Scales the image."

#define kFormatParamName "format"
#define kFormatParamLabel "Format"
#define kFormatParamHint "The output format"

#define kSizeParamName "size"
#define kSizeParamLabel "Size"
#define kSizeParamHint "The output size"

#define kPreserveParParamName "keepPAR"
#define kPreserveParParamLabel "Preserve aspect ratio"
#define kPreserveParParamHint "When checked, one direction will be clipped."

#define kScaleParamName "scale"
#define kScaleParamLabel "Scale"
#define kScaleParamHint "The scale factor to apply to the image."

#define kFilterParamName "filter"
#define kFilterParamLabel "Filter"
#define kFilterParamHint "The filter used to resize. Lanczos3 is great for downscaling and blackman-harris is great for upscaling."

using namespace OFX;
using namespace OpenImageIO;

class OIIOResizePlugin : public OFX::ImageEffect
{
public:

    OIIOResizePlugin(OfxImageEffectHandle handle);

    virtual ~OIIOResizePlugin();

    /* Override the render */
    virtual void render(const OFX::RenderArguments &args);

    /* override is identity */
    virtual bool isIdentity(const OFX::RenderArguments &args, OFX::Clip * &identityClip, double &identityTime);

    /* override changedParam */
    virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName);

    /* override changed clip */
    //virtual void changedClip(const OFX::InstanceChangedArgs &args, const std::string &clipName);

    // override the rod call
    virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod);

    // override the roi call
    virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args, OFX::RegionOfInterestSetter &rois);

private:
    
    template <typename PIX,int nComps>
    void renderInternal(const OFX::RenderArguments &args, TypeDesc srcType, const OFX::Image* srcImg, TypeDesc dstType, OFX::Image* dstImg);
    
    void fillWithBlack(OFX::PixelProcessorFilterBase & processor,
                       const OfxRectI &renderWindow,
                       void *dstPixelData,
                       const OfxRectI& dstBounds,
                       OFX::PixelComponentEnum dstPixelComponents,
                       OFX::BitDepthEnum dstPixelDepth,
                       int dstRowBytes);
    
    // do not need to delete these, the ImageEffect is managing them for us
    OFX::Clip *dstClip_;
    OFX::Clip *srcClip_;

    OFX::ChoiceParam *type_;
    OFX::ChoiceParam *format_;
    OFX::ChoiceParam *filter_;
    OFX::Int2DParam *size_;
    OFX::Double2DParam *scale_;
    OFX::BooleanParam *preservePAR_;
    
};

OIIOResizePlugin::OIIOResizePlugin(OfxImageEffectHandle handle)
: OFX::ImageEffect(handle)
, dstClip_(0)
, srcClip_(0)
, type_(0)
, format_(0)
, filter_(0)
, size_(0)
, scale_(0)
, preservePAR_(0)
{
    dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
    assert(dstClip_ && (dstClip_->getPixelComponents() == OFX::ePixelComponentRGBA ||
                        dstClip_->getPixelComponents() == OFX::ePixelComponentRGB ||
                        dstClip_->getPixelComponents() == OFX::ePixelComponentAlpha));
    srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
    assert(srcClip_ && (srcClip_->getPixelComponents() == OFX::ePixelComponentRGBA ||
                        srcClip_->getPixelComponents() == OFX::ePixelComponentRGB ||
                        srcClip_->getPixelComponents() == OFX::ePixelComponentAlpha));

    type_ = fetchChoiceParam(kTypeParamName);
    format_ = fetchChoiceParam(kFormatParamName);
    filter_ = fetchChoiceParam(kFilterParamName);
    size_ = fetchInt2DParam(kSizeParamName);
    scale_ = fetchDouble2DParam(kScaleParamName);
    preservePAR_ = fetchBooleanParam(kPreserveParParamName);
    assert(type_ && format_ &&  filter_ && size_ && scale_ && preservePAR_);
}

OIIOResizePlugin::~OIIOResizePlugin()
{
}

/* Override the render */
void
OIIOResizePlugin::render(const OFX::RenderArguments &args)
{
    std::auto_ptr<OFX::Image> dst(dstClip_->fetchImage(args.time));
    if (!dst.get()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    if (dst->getRenderScale().x != args.renderScale.x ||
        dst->getRenderScale().y != args.renderScale.y ||
        dst->getField() != args.fieldToRender) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    
    std::auto_ptr<OFX::Image> src(srcClip_->fetchImage(args.time));
    if (src.get()) {
        OFX::BitDepthEnum dstBitDepth       = dst->getPixelDepth();
        OFX::PixelComponentEnum dstComponents  = dst->getPixelComponents();
        assert(dstComponents == OFX::ePixelComponentRGB || dstComponents == OFX::ePixelComponentRGBA ||
               dstComponents == OFX::ePixelComponentAlpha);
        
        OFX::BitDepthEnum    srcBitDepth      = src->getPixelDepth();
        OFX::PixelComponentEnum srcComponents = src->getPixelComponents();
        if (srcBitDepth != dstBitDepth || srcComponents != dstComponents) {
            OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
        }

        if (dstComponents == OFX::ePixelComponentRGBA) {
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    renderInternal<unsigned char, 4>(args,TypeDesc::UCHAR,src.get(),TypeDesc::UCHAR, dst.get());
                }   break;
                case OFX::eBitDepthUShort: {
                    renderInternal<unsigned short, 4>(args,TypeDesc::USHORT,src.get(),TypeDesc::USHORT, dst.get());
                }   break;
                case OFX::eBitDepthFloat: {
                    renderInternal<float, 4>(args,TypeDesc::FLOAT,src.get(),TypeDesc::FLOAT, dst.get());
                }   break;
                default:
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
            }
        } else if (dstComponents == OFX::ePixelComponentRGB) {
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    renderInternal<unsigned char, 3>(args,TypeDesc::UCHAR,src.get(),TypeDesc::UCHAR, dst.get());
                }   break;
                case OFX::eBitDepthUShort: {
                    renderInternal<unsigned short, 3>(args,TypeDesc::USHORT,src.get(),TypeDesc::USHORT, dst.get());
                }   break;
                case OFX::eBitDepthFloat: {
                    renderInternal<float, 3>(args,TypeDesc::FLOAT,src.get(),TypeDesc::FLOAT, dst.get());
                }   break;
                default:
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
            }
        } else {
            assert(dstComponents == OFX::ePixelComponentAlpha);
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    renderInternal<unsigned char, 1>(args,TypeDesc::UCHAR,src.get(),TypeDesc::UCHAR, dst.get());
                }   break;
                case OFX::eBitDepthUShort: {
                    renderInternal<unsigned short, 1>(args,TypeDesc::USHORT,src.get(),TypeDesc::USHORT, dst.get());
                }   break;
                case OFX::eBitDepthFloat: {
                    renderInternal<float, 1>(args,TypeDesc::FLOAT,src.get(),TypeDesc::FLOAT, dst.get());
                }   break;
                default :
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
            }
        }
    } else { //!src.get()
        void* dstPixelData;
        OfxRectI dstBounds;
        PixelComponentEnum dstComponents;
        BitDepthEnum dstBitDepth;
        int dstRowBytes;
        getImageData(dst.get(), &dstPixelData, &dstBounds, &dstComponents, &dstBitDepth, &dstRowBytes);
        
        assert(dstComponents == OFX::ePixelComponentRGB || dstComponents == OFX::ePixelComponentRGBA ||
               dstComponents == OFX::ePixelComponentAlpha);
        
        if (dstComponents == OFX::ePixelComponentRGBA) {
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    BlackFiller<unsigned char, 4> proc(*this);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthUShort: {
                    BlackFiller<unsigned short, 4> proc(*this);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthFloat: {
                    BlackFiller<float, 4> proc(*this);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstBitDepth, dstRowBytes);
                }   break;
                default :
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
            }
        } else if (dstComponents == OFX::ePixelComponentRGB) {
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    BlackFiller<unsigned char, 3> proc(*this);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthUShort: {
                    BlackFiller<unsigned short, 3> proc(*this);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthFloat: {
                    BlackFiller<float, 3> proc(*this);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstBitDepth, dstRowBytes);
                }   break;
                default :
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
            }
        } else {
            assert(dstComponents == OFX::ePixelComponentAlpha);
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    BlackFiller<unsigned char, 1> proc(*this);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthUShort: {
                    BlackFiller<unsigned short, 1> proc(*this);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthFloat: {
                    BlackFiller<float, 1> proc(*this);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstBitDepth, dstRowBytes);
                }   break;
                default :
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
            }
        }
    }
}

template <typename PIX,int nComps>
void
OIIOResizePlugin::renderInternal(const OFX::RenderArguments &args,
                                 TypeDesc srcType,
                                 const OFX::Image* srcImg,
                                 TypeDesc dstType,
                                 OFX::Image* dstImg)
{
    ImageSpec srcSpec(srcType);
    const OfxRectI srcBounds = srcImg->getBounds();
    srcSpec.x = srcBounds.x1;
    srcSpec.y = srcBounds.y1;
    srcSpec.width = srcBounds.x2 - srcBounds.x1;
    srcSpec.height = srcBounds.y2 - srcBounds.y1;
    srcSpec.nchannels = nComps;
    srcSpec.full_x = srcSpec.x;
    srcSpec.full_y = srcSpec.y;
    srcSpec.full_width = srcSpec.width;
    srcSpec.full_height = srcSpec.height;
    srcSpec.default_channel_names();
    
    ImageBuf srcBuf(srcSpec,srcImg->getPixelAddress(srcBounds.x1, srcBounds.y1));
    
    
    ///This code assumes that the dstImg has the target size hence that we don't support tiles
    const OfxRectI dstBounds = dstImg->getBounds();
    ImageSpec dstSpec(dstType);
    dstSpec.x = dstBounds.x1;
    dstSpec.y = dstBounds.y1;
    dstSpec.width = dstBounds.x2 - dstBounds.x1;
    dstSpec.height = dstBounds.y2 - dstBounds.y1;
    dstSpec.nchannels = nComps;
    dstSpec.full_x = dstSpec.x;
    dstSpec.full_y = dstSpec.y;
    dstSpec.full_width = dstSpec.width;
    dstSpec.full_height = dstSpec.height;
    dstSpec.default_channel_names();
    
    ImageBuf dstBuf(dstSpec,dstImg->getPixelAddress(dstBounds.x1, dstBounds.y1));
    
    int filter;
    filter_->getValue(filter);

    if (filter == 0) {
        ///Use nearest neighboor
        if (!ImageBufAlgo::resample(dstBuf, srcBuf,/*interpolate*/false)) {
            setPersistentMessage(OFX::Message::eMessageError, "", dstBuf.geterror());
        }
    } else {
        ///interpolate using the selected filter
        FilterDesc f;
        Filter2D::get_filterdesc(filter - 1, &f);
        if (!ImageBufAlgo::resize(dstBuf, srcBuf, f.name,f.width)) {
            setPersistentMessage(OFX::Message::eMessageError, "", dstBuf.geterror());
        }
    }
}

void
OIIOResizePlugin::fillWithBlack(OFX::PixelProcessorFilterBase & processor,
                                const OfxRectI &renderWindow,
                                void *dstPixelData,
                                const OfxRectI& dstBounds,
                                OFX::PixelComponentEnum dstPixelComponents,
                                OFX::BitDepthEnum dstPixelDepth,
                                int dstRowBytes)
{
    // set the images
    processor.setDstImg(dstPixelData, dstBounds, dstPixelComponents, dstPixelDepth, dstRowBytes);
    
    // set the render window
    processor.setRenderWindow(renderWindow);
    
    // Call the base class process member, this will call the derived templated process code
    processor.process();
    
}


bool
OIIOResizePlugin::isIdentity(const OFX::RenderArguments &args,
                             OFX::Clip * &identityClip,
                             double &/*identityTime*/)
{
    int type;
    type_->getValue(type);
    switch (type) {
        case 0: {
            OfxRectD srcRoD = srcClip_->getRegionOfDefinition(args.time);
            int index;
            format_->getValue(index);
            double par;
            size_t w,h;
            getFormatResolution((OFX::EParamFormat)index, &w, &h, &par);
            if (srcRoD.x1 == 0 && srcRoD.y1 == 0 && srcRoD.x2 == w && srcRoD.y2 == h) {
                return true;
            }
            return false;
        }   break;
        case 1: {
            OfxRectD srcRoD = srcClip_->getRegionOfDefinition(args.time);
            int w,h;
            size_->getValue(w, h);
            if (srcRoD.x1 == 0 && srcRoD.y1 == 0 && srcRoD.x2 == w && srcRoD.y2 == h) {
                return true;
            }
            return false;
        }   break;
        case 2: {
            double sx,sy;
            scale_->getValue(sx, sy);
            if (sx == 1. && sy == 1.) {
                return true;
            }
            return false;
        }   break;

        default:
            break;
    }
    return false;
}


void
OIIOResizePlugin::changedParam(const OFX::InstanceChangedArgs &args,
                               const std::string &paramName)
{
    if (paramName == kTypeParamName) {
        int type;
        type_->getValue(type);
        switch (type) {
            case 0: {//specific output format
                size_->setIsSecret(true);
                preservePAR_->setIsSecret(true);
                scale_->setIsSecret(true);
                format_->setIsSecret(false);
            }   break;
            case 1: {//size
                size_->setIsSecret(false);
                preservePAR_->setIsSecret(false);
                scale_->setIsSecret(true);
                format_->setIsSecret(true);
            }   break;
                
            case 2:  {//scaled
                size_->setIsSecret(true);
                preservePAR_->setIsSecret(true);
                scale_->setIsSecret(false);
                format_->setIsSecret(true);
            }   break;
            default:
                break;
        }
    }
}

bool
OIIOResizePlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args,
                                        OfxRectD &rod)
{
    int type;
    type_->getValue(type);
    switch (type) {
        case 0: {//specific output format
            int index;
            format_->getValue(index);
            double par;
            size_t w,h;
            getFormatResolution((OFX::EParamFormat)index, &w, &h, &par);
            rod.x1 = rod.y1 = 0;
            rod.x2 = w;
            rod.y2 = h;
        }   break;
        case 1: {//size
            int w,h;
            size_->getValue(w, h);
            bool preservePar;
            preservePAR_->getValue(preservePar);
            if (preservePar) {
                OfxRectD srcRoD = srcClip_->getRegionOfDefinition(args.time);
                double srcW = srcRoD.x2 - srcRoD.x1;
                double srcH = srcRoD.y2 - srcRoD.y1 ;
                
                ///Don't crash if we were provided weird RoDs
                if (srcH < 1 || srcW < 1) {
                    return false;
                }
                if ((double)w / srcW < (double)h / srcH) {
                    ///Keep the given width, recompute the height
                    h = srcH * w / srcW;
                } else {
                    ///Keep the given height,recompute the width
                    w = srcW * h / srcH;
                }
                
            }
            rod.x1 = 0;
            rod.y1 = 0;
            rod.x2 = w;
            rod.y2 = h;
        }   break;
            
        case 2:  {//scaled
            OfxRectD srcRoD = srcClip_->getRegionOfDefinition(args.time);
            double sx,sy;
            scale_->getValue(sx, sy);
            rod.x1 = srcRoD.x1 * sx;
            rod.y1 = srcRoD.y1 * sy;
            rod.x2 = srcRoD.x2 * sx;
            rod.y2 = srcRoD.y2 * sy;
        }   break;
        default:
            return false;
    }
    return true;
}

// override the roi call
void
OIIOResizePlugin::getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args,
                                       OFX::RegionOfInterestSetter &rois)
{
    if (!kSupportsTiles) {
        // The effect requires full images to render any region
        OfxRectD srcRoI;

        if (srcClip_ && srcClip_->isConnected()) {
            srcRoI = srcClip_->getRegionOfDefinition(args.time);
            rois.setRegionOfInterest(*srcClip_, srcRoI);
        }
    }
}


using namespace OFX;

mDeclarePluginFactory(OIIOResizePluginFactory, {}, {});

/** @brief The basic describe function, passed a plugin descriptor */
void OIIOResizePluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    
    // Let OIIO do the multi-threading for us
//    This attribute sets the
//    maximum number of threads that will be spawned. The default is 1. If set to 0, it
//    means that it should use as many threads as there are hardware cores present on the
//    system.
    OIIO::attribute("threads", 0);
    
    // basic labels
    desc.setLabels(kPluginName, kPluginName, kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    // add the supported contexts
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextFilter);

    // add supported pixel depths
    desc.addSupportedBitDepth(OFX::eBitDepthUByte);
    desc.addSupportedBitDepth(OFX::eBitDepthUShort);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    
    ///We don't support tiles: we can only resize the whole RoD at once
    desc.setSupportsTiles(kSupportsTiles);
    
    ///We do support multiresolution
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    
    desc.setRenderThreadSafety(kRenderThreadSafety);
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void OIIOResizePluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, ContextEnum context)
{
    // Source clip only in the filter context
    // create the mandated source clip
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->addSupportedComponent(ePixelComponentAlpha);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(true);
    srcClip->setIsMask(false);

    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->addSupportedComponent(ePixelComponentAlpha);
    dstClip->setSupportsTiles(true);

    // make some pages and to things in
    PageParamDescriptor *page = desc.definePageParam("Controls");

    ChoiceParamDescriptor* type = desc.defineChoiceParam(kTypeParamName);
    type->setLabels(kTypeParamLabel, kTypeParamLabel, kTypeParamLabel);
    type->setHint(kTypeParamHint);
    type->appendOption("Format");
    type->appendOption("Size");
    type->appendOption("Scale");
    type->setAnimates(false);
    type->setDefault(0);
    page->addChild(*type);
    
    ChoiceParamDescriptor* format = desc.defineChoiceParam(kFormatParamName);
    format->setLabels(kFormatParamLabel, kFormatParamLabel, kFormatParamLabel);
    format->setAnimates(false);
    format->appendOption(kParamFormatPCVideoLabel);
    format->appendOption(kParamFormatNTSCLabel);
    format->appendOption(kParamFormatPALLabel);
    format->appendOption(kParamFormatHDLabel);
    format->appendOption(kParamFormatNTSC169Label);
    format->appendOption(kParamFormatPAL169Label);
    format->appendOption(kParamFormat1kSuper35Label);
    format->appendOption(kParamFormat1kCinemascopeLabel);
    format->appendOption(kParamFormat2kSuper35Label);
    format->appendOption(kParamFormat2kCinemascopeLabel);
    format->appendOption(kParamFormat4kSuper35Label);
    format->appendOption(kParamFormat4kCinemascopeLabel);
    format->appendOption(kParamFormatSquare256Label);
    format->appendOption(kParamFormatSquare512Label);
    format->appendOption(kParamFormatSquare1kLabel);
    format->appendOption(kParamFormatSquare2kLabel);
    format->setDefault(0);
    format->setHint(kFormatParamHint);
    page->addChild(*format);
    
    Int2DParamDescriptor* size = desc.defineInt2DParam(kSizeParamName);
    size->setLabels(kSizeParamLabel, kSizeParamLabel, kSizeParamLabel);
    size->setHint(kSizeParamHint);
    size->setDefault(200, 200);
    size->setAnimates(false);
    size->setIsSecret(true);
    size->setLayoutHint(eLayoutHintNoNewLine);
    page->addChild(*size);
    
    BooleanParamDescriptor* preservePAR = desc.defineBooleanParam(kPreserveParParamName);
    preservePAR->setLabels(kPreserveParParamLabel, kPreserveParParamLabel, kPreserveParParamLabel);
    preservePAR->setHint(kPreserveParParamHint);
    preservePAR->setAnimates(false);
    preservePAR->setDefault(false);
    preservePAR->setIsSecret(true);
    preservePAR->setDefault(true);
    page->addChild(*preservePAR);
    
    Double2DParamDescriptor* scale = desc.defineDouble2DParam(kScaleParamName);
    scale->setHint(kScaleParamHint);
    scale->setLabels(kScaleParamLabel, kScaleParamLabel, kScaleParamLabel);
    scale->setAnimates(true);
    scale->setIsSecret(true);
    scale->setDefault(1., 1.);
    scale->setIncrement(0.05);
    page->addChild(*scale);
    
    ChoiceParamDescriptor *filter = desc.defineChoiceParam(kFilterParamName);
    filter->setLabels(kFilterParamLabel, kFilterParamLabel, kFilterParamLabel);
    filter->setHint(kFilterParamHint);
    filter->setAnimates(false);
    filter->appendOption("Impulse (no interpolation)");
    int nFilters = Filter2D::num_filters();
    int defIndex = 0;
    for (int i = 0; i < nFilters; ++i) {
        FilterDesc f;
        Filter2D::get_filterdesc(i, &f);
        filter->appendOption(f.name);
        if (!strcmp(f.name , "lanczos3")) {
            defIndex = i + 1; // +1 because we added the "impulse" option
        }
    }
    filter->setDefault(defIndex);
    page->addChild(*filter);
}

/** @brief The create instance function, the plugin must return an object derived from the \ref OFX::ImageEffect class */
ImageEffect* OIIOResizePluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new OIIOResizePlugin(handle);
}


void getOIIOResizePluginID(OFX::PluginFactoryArray &ids)
{
    static OIIOResizePluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}

#endif // DEBUG

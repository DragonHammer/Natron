/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2013-2017 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "OfxEffectInstance.h"

#include <locale>
#include <limits>
#include <cassert>
#include <stdexcept>

#include <QtCore/QDebug>
#include <QtCore/QByteArray>
#include <QtCore/QReadWriteLock>
#include <QtCore/QPointF>

// ofxhPropertySuite.h:565:37: warning: 'this' pointer cannot be null in well-defined C++ code; comparison may be assumed to always evaluate to true [-Wtautological-undefined-compare]
CLANG_DIAG_OFF(unknown-pragmas)
CLANG_DIAG_OFF(tautological-undefined-compare) // appeared in clang 3.5
#include <ofxhPluginCache.h>
#include <ofxhPluginAPICache.h>
#include <ofxhImageEffect.h>
#include <ofxhImageEffectAPI.h>
#include <ofxOpenGLRender.h>
#include <ofxhHost.h>
CLANG_DIAG_ON(tautological-undefined-compare)
CLANG_DIAG_ON(unknown-pragmas)

#include <tuttle/ofxReadWrite.h>
#include <ofxNatron.h>
#include <nuke/fnOfxExtensions.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Distortion2D.h"
#include "Engine/EffectInstanceTLSData.h"
#include "Engine/EffectOpenGLContextData.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/OfxClipInstance.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/OfxOverlayInteract.h"
#include "Engine/OfxParamInstance.h"
#include "Engine/OSGLContext.h"
#include "Engine/Project.h"
#include "Engine/ReadNode.h"
#include "Engine/RotoLayer.h"
#include "Engine/TimeLine.h"
#include "Engine/TLSHolder.h"
#include "Engine/Transform.h"
#include "Engine/TreeRender.h"
#include "Engine/UndoCommand.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"
#include "Engine/WriteNode.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER;

struct OfxEffectInstanceCommon
{
    // The internal OpenFX image effect is shared amongst clones because cloning for render
    // is not yet standardized.
    boost::scoped_ptr<OfxImageEffectInstance> effect;

    boost::scoped_ptr<OfxOverlayInteract> overlayInteract; // ptr to the overlay interact if any

    struct ClipsInfo
    {
        ClipsInfo()
        : optional(false)
        , mask(false)
        , clip(NULL)
        , label()
        , hint()
        , visible(true)
        , canReceiveDistortion(false)
        , canReceiveDeprecatedTransform3x3(false)
        {
        }

        bool optional;
        bool mask;
        OfxClipInstance* clip;
        std::string label;
        std::string hint;
        bool visible;
        bool canReceiveDistortion;
        bool canReceiveDeprecatedTransform3x3;
    };

    std::vector<ClipsInfo> clipsInfos;
    OfxClipInstance* outputClip;

    boost::shared_ptr<TLSHolder<EffectInstanceTLSData> > tlsData;

    KnobStringWPtr cursorKnob; // secret knob for ofx effects so they can set the cursor
    KnobIntWPtr selectionRectangleStateKnob;
    KnobStringWPtr undoRedoTextKnob;
    KnobBoolWPtr undoRedoStateKnob;
    ContextEnum context;

    int nbSourceClips;
    SequentialPreferenceEnum sequentialPref;
    mutable QMutex supportsConcurrentGLRendersMutex;
    bool supportsConcurrentGLRenders;
    bool isOutput; //if the OfxNode can output a file somehow
    bool penDown; // true when the overlay trapped a penDow action
    bool initialized; //true when the image effect instance has been created and populated

    /*
     Some OpenFX do not handle render scale properly when it comes to overlay interacts.
     We try to keep a blacklist of these and call overlay actions with render scale = 1  in that
     case
     */
    bool overlaysCanHandleRenderScale;
    bool supportsMultipleClipPARs;
    bool supportsMultipleClipDepths;
    bool doesTemporalAccess;
    bool multiplanar;

    OfxEffectInstanceCommon()
    : effect()
    , overlayInteract()
    , clipsInfos()
    , outputClip(0)
    , tlsData(new TLSHolder<EffectInstanceTLSData>())
    , cursorKnob()
    , selectionRectangleStateKnob()
    , undoRedoTextKnob()
    , undoRedoStateKnob()
    , context(eContextNone)
    , nbSourceClips(0)
    , sequentialPref(eSequentialPreferenceNotSequential)
    , supportsConcurrentGLRendersMutex()
    , supportsConcurrentGLRenders(false)
    , isOutput(false)
    , penDown(false)
    , initialized(false)
    , overlaysCanHandleRenderScale(true)
    , supportsMultipleClipPARs(false)
    , supportsMultipleClipDepths(false)
    , doesTemporalAccess(false)
    , multiplanar(false)
    {

    }
};

struct OfxEffectInstancePrivate
{

    boost::shared_ptr<OfxEffectInstanceCommon> common;

    OfxEffectInstancePrivate()
    : common()
    {
    }


};



ThreadIsActionCaller_RAII::ThreadIsActionCaller_RAII(const OfxEffectInstancePtr& effect)
{
    appPTR->setOFXLastActionCaller_TLS(effect);
}

ThreadIsActionCaller_RAII::~ThreadIsActionCaller_RAII()
{
    appPTR->setOFXLastActionCaller_TLS(OfxEffectInstancePtr());
}


OfxEffectInstance::OfxEffectInstance(const NodePtr& node)
: AbstractOfxEffectInstance(node)
, _imp(new OfxEffectInstancePrivate)
{
    _imp->common.reset(new OfxEffectInstanceCommon);
    QObject::connect( this, SIGNAL(syncPrivateDataRequested()), this, SLOT(onSyncPrivateDataRequested()) );
}


OfxEffectInstance::OfxEffectInstance(const EffectInstancePtr& mainInstance, const TreeRenderPtr& render)
: AbstractOfxEffectInstance(mainInstance, render)
, _imp(new OfxEffectInstancePrivate)
{
    OfxEffectInstance* mainEffect = dynamic_cast<OfxEffectInstance*>(mainInstance.get());
    assert(mainEffect);
    _imp->common = mainEffect->_imp->common;
}

void
OfxEffectInstance::describePlugin()
{


    PluginPtr natronPlugin = getNode()->getPlugin();
    assert(natronPlugin);

    OFX::Host::ImageEffect::ImageEffectPlugin* ofxPlugin = (OFX::Host::ImageEffect::ImageEffectPlugin*)natronPlugin->getPropertyUnsafe<void*>(kNatronPluginPropOpenFXPluginPtr);
    assert(ofxPlugin);
    if (!ofxPlugin) {
        throw std::logic_error("OfxEffectInstance::describePlugin kNatronPluginPropOpenFXPluginPtr is NULL");
    }

    // Check if we already called describe then describeInContext.
    OFX::Host::ImageEffect::Descriptor* desc = natronPlugin->getOfxDesc(&_imp->common->context);

    if (!desc) {
        // Call the actions
        try {
            //  Should this method be in AppManager?
            desc = appPTR->getPluginContextAndDescribe(ofxPlugin, &_imp->common->context);
        } catch (const std::exception& e) {
            std::string message = tr("Failed to create an instance of %1:").arg(QString::fromUtf8(natronPlugin->getPluginID().c_str())).toStdString()
            + '\n' + e.what();
            throw std::runtime_error(message);
        }

        assert(desc);
        natronPlugin->setOfxDesc(desc, _imp->common->context);

    }

    assert(ofxPlugin && desc && _imp->common->context != eContextNone);

    if (_imp->common->context == eContextWriter) {
        _imp->common->isOutput = true;
    }



    _imp->common->effect.reset( new OfxImageEffectInstance(ofxPlugin, *desc, mapContextToString(_imp->common->context), false) );
    assert(_imp->common->effect);

    OfxEffectInstancePtr thisShared = boost::dynamic_pointer_cast<OfxEffectInstance>( shared_from_this() );
    _imp->common->effect->setOfxEffectInstance(thisShared);

    OfxEffectInstance::MappedInputV clips = inputClipsCopyWithoutOutput();
    _imp->common->nbSourceClips = (int)clips.size();

    _imp->common->clipsInfos.resize( clips.size() );
    for (unsigned i = 0; i < clips.size(); ++i) {
        OfxEffectInstanceCommon::ClipsInfo info;
        info.optional = clips[i]->isOptional();
        info.mask = clips[i]->isMask();
        info.clip = NULL;
        // label, hint, visible are set below
        _imp->common->clipsInfos[i] = info;
    }

    _imp->common->supportsMultipleClipPARs = _imp->common->effect->supportsMultipleClipPARs();
    _imp->common->supportsMultipleClipDepths = _imp->common->effect->supportsMultipleClipDepths();
    _imp->common->doesTemporalAccess = _imp->common->effect->temporalAccess();
    _imp->common->multiplanar = _imp->common->effect->isMultiPlanar();
    int sequential = _imp->common->effect->getPlugin()->getDescriptor().getProps().getIntProperty(kOfxImageEffectInstancePropSequentialRender);
    switch (sequential) {
        case 0:
            _imp->common->sequentialPref = eSequentialPreferenceNotSequential;
            break;
        case 1:
            _imp->common->sequentialPref = eSequentialPreferenceOnlySequential;
            break;
        case 2:
            _imp->common->sequentialPref = eSequentialPreferencePreferSequential;
            break;
        default:
            _imp->common->sequentialPref =  eSequentialPreferenceNotSequential;
            break;
    }

    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

    ///Create clips & parameters
    OfxStatus stat = _imp->common->effect->populate();
    if (stat != kOfxStatOK) {
        throw std::runtime_error("Failed to create parameters and clips");
    }

    for (unsigned i = 0; i < clips.size(); ++i) {
        _imp->common->clipsInfos[i].clip = dynamic_cast<OfxClipInstance*>( _imp->common->effect->getClip( clips[i]->getName() ) );
        _imp->common->clipsInfos[i].label = _imp->common->clipsInfos[i].clip->getLabel();
        _imp->common->clipsInfos[i].hint = _imp->common->clipsInfos[i].clip->getHint();
        _imp->common->clipsInfos[i].visible = !_imp->common->clipsInfos[i].clip->isSecret();
        _imp->common->clipsInfos[i].canReceiveDistortion = clips[i]->canDistort();
        _imp->common->clipsInfos[i].canReceiveDeprecatedTransform3x3 = clips[i]->canTransform();

        // An effect that supports the distortion suite should not supports also the old transformation suite: this is obsolete.
        assert(!_imp->common->clipsInfos[i].canReceiveDistortion || !_imp->common->clipsInfos[i].canReceiveDeprecatedTransform3x3);
        assert(_imp->common->clipsInfos[i].clip);
    }

    _imp->common->outputClip = dynamic_cast<OfxClipInstance*>( _imp->common->effect->getClip(kOfxImageEffectOutputClipName) );
    assert(_imp->common->outputClip);

    _imp->common->effect->addParamsToTheirParents();

    {
        KnobIPtr foundCursorKnob = getKnobByName(kNatronOfxParamCursorName);
        if (foundCursorKnob) {
            KnobStringPtr isStringKnob = toKnobString(foundCursorKnob);
            _imp->common->cursorKnob = isStringKnob;
        }
    }

    {

        KnobIPtr foundSelKnob = getKnobByName(kNatronOfxImageEffectSelectionRectangle);
        if (foundSelKnob) {
            KnobIntPtr isIntKnob = toKnobInt(foundSelKnob);
            _imp->common->selectionRectangleStateKnob = isIntKnob;
        }
    }
    {
        KnobIPtr foundTextKnob = getKnobByName(kNatronOfxParamUndoRedoText);
        if (foundTextKnob) {
            KnobStringPtr isStringKnob = toKnobString(foundTextKnob);
            _imp->common->undoRedoTextKnob = isStringKnob;
        }
    }
    {
        KnobIPtr foundUndoRedoKnob = getKnobByName(kNatronOfxParamUndoRedoState);
        if (foundUndoRedoKnob) {
            KnobBoolPtr isBool = toKnobBool(foundUndoRedoKnob);
            _imp->common->undoRedoStateKnob = isBool;
        }
    }


    int nPages = _imp->common->effect->getDescriptor().getProps().getDimension(kOfxPluginPropParamPageOrder);
    std::list<std::string> pagesOrder;
    for (int i = 0; i < nPages; ++i) {
        const std::string& pageName = _imp->common->effect->getDescriptor().getProps().getStringProperty(kOfxPluginPropParamPageOrder, i);
        pagesOrder.push_back(pageName);
    }
    if ( !pagesOrder.empty() ) {
        getNode()->setPagesOrder(pagesOrder);
    }


    assert( _imp->common->effect->getPlugin() );
    assert( _imp->common->effect->getPlugin()->getPluginHandle() );
    assert( _imp->common->effect->getPlugin()->getPluginHandle()->getOfxPlugin() );
    assert(_imp->common->effect->getPlugin()->getPluginHandle()->getOfxPlugin()->mainEntry);

} // describePlugin


void
OfxEffectInstance::createInstanceAction()
{

    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
    
    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
    assert(!tls->isCurrentAction(kOfxActionCreateInstance));
    EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxActionCreateInstance, TimeValue(getApp()->getTimeLine()->currentFrame()), ViewIdx(0), RenderScale(1.)
#ifdef DEBUG
                                              , /*canSetValue*/ true
                                              , /*canBeCalledRecursively*/ false
#endif
                                              );


    OfxStatus stat;
    stat = _imp->common->effect->createInstanceAction();

    if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
        QString message;
        int type;
        NodePtr messageContainer = getNode();
        NodePtr ioContainer = messageContainer->getIOContainer();
        if (ioContainer) {
            messageContainer = ioContainer;
        }
        messageContainer->getPersistentMessage(&message, &type);
        if (message.isEmpty()) {
            throw std::runtime_error(tr("Could not create effect instance for plugin").toStdString());
        } else {
            throw std::runtime_error(message.toStdString());
        }
    }

    _imp->common->initialized = true;

}

OfxImageEffectInstance*
OfxEffectInstance::effectInstance()
{
    return _imp->common->effect.get();
}

const OfxImageEffectInstance*
OfxEffectInstance::effectInstance() const
{
    return _imp->common->effect.get();
}

bool
OfxEffectInstance::isInitialized() const
{
    return _imp->common->initialized;
}


OfxEffectInstance::~OfxEffectInstance()
{
    if (_imp->common.use_count() == 1 && _imp->common->effect) {
        _imp->common->overlayInteract.reset();
        _imp->common->effect->destroyInstanceAction();
    }
}

#ifdef DEBUG
void
OfxEffectInstance::checkCanSetValueAndWarn()
{
    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getTLSData();
    if (!tls) {
        return;
    }
    if (tls->isDuringActionThatCannotSetValue()) {
        qDebug() << getScriptName_mt_safe().c_str() << ": setValue()/setValueAtTime() was called during an action that is not allowed to call this function.";
    }
}
#endif

EffectInstanceTLSDataPtr
OfxEffectInstance::getTLSObject() const
{
    return _imp->common->tlsData->getTLSData();
}

EffectInstanceTLSDataPtr
OfxEffectInstance::getTLSObjectForThread(QThread* thread) const
{
    return _imp->common->tlsData->getTLSDataForThread(thread);
}

EffectInstanceTLSDataPtr
OfxEffectInstance::getOrCreateTLSObject() const
{
    return _imp->common->tlsData->getOrCreateTLSData();
}


void
OfxEffectInstance::tryInitializeOverlayInteracts()
{
    assert(_imp->common->context != eContextNone);
    if (_imp->common->overlayInteract) {
        // already created
        return;
    }
    
    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

    QString pluginID = QString::fromUtf8( getNode()->getPluginID().c_str() );
    /*
       Currently genarts plug-ins do not handle render scale properly for overlays
     */
    if ( pluginID.startsWith( QString::fromUtf8("com.genarts.") ) ) {
        _imp->common->overlaysCanHandleRenderScale = false;
    }

    /*create overlay instance if any*/
    assert(_imp->common->effect);
    OfxPluginEntryPoint *overlayEntryPoint = _imp->common->effect->getOverlayInteractMainEntry();
    if (overlayEntryPoint) {
        _imp->common->overlayInteract.reset( new OfxOverlayInteract(*_imp->common->effect, 8, true) );
        double sx, sy;
        effectInstance()->getRenderScaleRecursive(sx, sy);
        RenderScale s(sx, sy);


        _imp->common->overlayInteract->createInstanceAction();


        ///Fetch all parameters that are overlay slave
        std::vector<std::string> slaveParams;
        _imp->common->overlayInteract->getSlaveToParam(slaveParams);
        for (U32 i = 0; i < slaveParams.size(); ++i) {
            KnobIPtr param;
            const std::vector< KnobIPtr > & knobs = getKnobs();
            for (std::vector< KnobIPtr >::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
                if ( (*it)->getOriginalName() == slaveParams[i] ) {
                    param = *it;
                    break;
                }
            }
            if (!param) {
                qDebug() << "OfxEffectInstance::tryInitializeOverlayInteracts(): slaveToParam " << slaveParams[i].c_str() << " not available";
            } else {
                addOverlaySlaveParam(param);
            }
        }

        getApp()->redrawAllViewers();
    }


    ///for each param, if it has a valid custom interact, create it
    const std::list<OFX::Host::Param::Instance*> & params = effectInstance()->getParamList();
    for (std::list<OFX::Host::Param::Instance*>::const_iterator it = params.begin(); it != params.end(); ++it) {
        OfxParamToKnob* paramToKnob = dynamic_cast<OfxParamToKnob*>(*it);
        assert(paramToKnob);
        if (!paramToKnob) {
            continue;
        }

        OfxPluginEntryPoint* interactEntryPoint = paramToKnob->getCustomOverlayInteractEntryPoint(*it);
        if (!interactEntryPoint) {
            continue;
        }
        KnobIPtr knob = paramToKnob->getKnob();
        const OFX::Host::Property::PropSpec* interactDescProps = OfxImageEffectInstance::getOfxParamOverlayInteractDescProps();
        OFX::Host::Interact::Descriptor &interactDesc = paramToKnob->getInteractDesc();
        interactDesc.getProperties().addProperties(interactDescProps);
        interactDesc.setEntryPoint(interactEntryPoint);

        int bitdepthPerComponent = 0;
        bool hasAlpha = false;
        getApp()->getViewersOpenGLContextFormat(&bitdepthPerComponent, &hasAlpha);
        interactDesc.describe(bitdepthPerComponent, hasAlpha);
        OfxParamOverlayInteractPtr overlayInteract( new OfxParamOverlayInteract( knob, interactDesc, effectInstance()->getHandle()) );

        knob->setCustomInteract(overlayInteract);
        overlayInteract->createInstanceAction();
    }
} // OfxEffectInstance::tryInitializeOverlayInteracts

void
OfxEffectInstance::setInteractColourPicker(const OfxRGBAColourD& color, bool setColor, bool hasColor)
{
    if (!_imp->common->overlayInteract) {
        return;
    }

    if (!_imp->common->overlayInteract->isColorPickerRequired()) {
        return;
    }
    if (!hasColor) {
        _imp->common->overlayInteract->setHasColorPicker(false);
    } else {
        if (setColor) {
            _imp->common->overlayInteract->setLastColorPickerColor(color);
        }
        _imp->common->overlayInteract->setHasColorPicker(true);
    }

    ignore_result(_imp->common->overlayInteract->redraw());
}

bool
OfxEffectInstance::isOutput() const
{
    assert(_imp->common->context != eContextNone);

    return _imp->common->isOutput;
}

bool
OfxEffectInstance::isGenerator() const
{
#if 1
    /*
     * This is to deal with effects that can be both filters and generators (e.g: like constant or S_Zap)
     * Some plug-ins unfortunately do not behave exactly the same in these 2 contexts and we want them to behave
     * as a general context. So we just look for the presence of the generator context to determine if the plug-in
     * is really a generator or not.
     */

    assert( effectInstance() );
    const std::set<std::string> & contexts = effectInstance()->getPlugin()->getContexts();
    std::set<std::string>::const_iterator foundGenerator = contexts.find(kOfxImageEffectContextGenerator);
    std::set<std::string>::const_iterator foundReader = contexts.find(kOfxImageEffectContextReader);
    if ( ( foundGenerator != contexts.end() ) || ( foundReader != contexts.end() ) ) {
        return true;
    }

    return false;
#else
    assert(_context != eContextNone);

    return _context == eContextGenerator || _context == eContextReader;
#endif
}

bool
OfxEffectInstance::isReader() const
{
    assert(_imp->common->context != eContextNone);

    return _imp->common->context == eContextReader;
}

bool
OfxEffectInstance::isVideoReader() const
{
    return isReader() && ReadNode::isVideoReader( getNode()->getPluginID() );
}


bool
OfxEffectInstance::isVideoWriter() const
{
    return isWriter() && WriteNode::isVideoWriter( getNode()->getPluginID() );
}

bool
OfxEffectInstance::isWriter() const
{
    assert(_imp->common->context != eContextNone);

    return _imp->common->context == eContextWriter;
}

bool
OfxEffectInstance::isGeneratorAndFilter() const
{
    assert(_imp->common->context != eContextNone);
    const std::set<std::string> & contexts = effectInstance()->getPlugin()->getContexts();
    std::set<std::string>::const_iterator foundGenerator = contexts.find(kOfxImageEffectContextGenerator);
    std::set<std::string>::const_iterator foundGeneral = contexts.find(kOfxImageEffectContextGeneral);

    return foundGenerator != contexts.end() && foundGeneral != contexts.end();
}

/*group is a string as such:
   Toto/Superplugins/blabla
   This functions extracts the all parts of such a grouping, e.g in this case
   it would return [Toto,Superplugins,blabla].*/
static
QStringList
ofxExtractAllPartsOfGrouping(const QString & pluginIdentifier,
                             int /*versionMajor*/,
                             int /*versionMinor*/,
                             const QString & /*pluginLabel*/,
                             const QString & str)
{
    QString s(str);
    std::string stdIdentifier = pluginIdentifier.toStdString();

    s.replace( QLatin1Char('\\'), QLatin1Char('/') );

    QStringList out;
    if ( ( pluginIdentifier.startsWith( QString::fromUtf8("com.genarts.sapphire.") ) || s.startsWith( QString::fromUtf8("Sapphire ") ) || s.startsWith( QString::fromUtf8(" Sapphire ") ) ) &&
         !s.startsWith( QString::fromUtf8("Sapphire/") ) ) {
        out.push_back( QString::fromUtf8("Sapphire") );
    } else if ( ( pluginIdentifier.startsWith( QString::fromUtf8("com.genarts.monsters.") ) || s.startsWith( QString::fromUtf8("Monsters ") ) || s.startsWith( QString::fromUtf8(" Monsters ") ) ) &&
                !s.startsWith( QString::fromUtf8("Monsters/") ) ) {
        out.push_back( QString::fromUtf8("Monsters") );
    } else if ( ( pluginIdentifier == QString::fromUtf8("uk.co.thefoundry.keylight.keylight") ) ||
                ( pluginIdentifier == QString::fromUtf8("jp.co.ise.imagica:PrimattePlugin") ) ) {
        s = QString::fromUtf8(PLUGIN_GROUP_KEYER);
    } else if ( ( pluginIdentifier == QString::fromUtf8("uk.co.thefoundry.noisetools.denoise") ) ||
                pluginIdentifier.startsWith( QString::fromUtf8("com.rubbermonkey:FilmConvert") ) ) {
        s = QString::fromUtf8(PLUGIN_GROUP_FILTER);
    } else if ( pluginIdentifier.startsWith( QString::fromUtf8("com.NewBlue.Titler") ) ) {
        s = QString::fromUtf8(PLUGIN_GROUP_PAINT);
    } else if ( pluginIdentifier.startsWith( QString::fromUtf8("com.FXHOME.HitFilm") ) ) {
        // HitFilm uses grouping such as "HitFilm - Keying - Matte Enhancement"
        s.replace( QString::fromUtf8(" - "), QString::fromUtf8("/") );
    } else if ( pluginIdentifier.startsWith( QString::fromUtf8("com.redgiantsoftware.Universe") ) && s.startsWith( QString::fromUtf8("Universe ") ) ) {
        // Red Giant Universe uses grouping such as "Universe Blur"
        out.push_back( QString::fromUtf8("Universe") );
    } else if ( pluginIdentifier.startsWith( QString::fromUtf8("com.NewBlue.") ) && s.startsWith( QString::fromUtf8("NewBlue ") ) ) {
        // NewBlueFX uses grouping such as "NewBlue Elements"
        out.push_back( QString::fromUtf8("NewBlue") );
    } else if ( (stdIdentifier == "tuttle.avreader") ||
                (stdIdentifier == "tuttle.avwriter") ||
                (stdIdentifier == "tuttle.dpxwriter") ||
                (stdIdentifier == "tuttle.exrreader") ||
                (stdIdentifier == "tuttle.exrwriter") ||
                (stdIdentifier == "tuttle.imagemagickreader") ||
                (stdIdentifier == "tuttle.jpeg2000reader") ||
                (stdIdentifier == "tuttle.jpeg2000writer") ||
                (stdIdentifier == "tuttle.jpegreader") ||
                (stdIdentifier == "tuttle.jpegwriter") ||
                (stdIdentifier == "tuttle.oiioreader") ||
                (stdIdentifier == "tuttle.oiiowriter") ||
                (stdIdentifier == "tuttle.pngreader") ||
                (stdIdentifier == "tuttle.pngwriter") ||
                (stdIdentifier == "tuttle.rawreader") ||
                (stdIdentifier == "tuttle.turbojpegreader") ||
                (stdIdentifier == "tuttle.turbojpegwriter") ) {
        out.push_back( QString::fromUtf8(PLUGIN_GROUP_IMAGE) );
        if ( pluginIdentifier.endsWith( QString::fromUtf8("reader") ) ) {
            s = QString::fromUtf8(PLUGIN_GROUP_IMAGE_READERS);
        } else {
            s = QString::fromUtf8(PLUGIN_GROUP_IMAGE_WRITERS);
        }
    } else if ( (stdIdentifier == "tuttle.checkerboard") ||
                (stdIdentifier == "tuttle.colorbars") ||
                (stdIdentifier == "tuttle.colorcube") || // TuttleColorCube
                (stdIdentifier == "tuttle.colorgradient") ||
                (stdIdentifier == "tuttle.colorwheel") ||
                (stdIdentifier == "tuttle.constant") ||
                (stdIdentifier == "tuttle.inputbuffer") ||
                (stdIdentifier == "tuttle.outputbuffer") ||
                (stdIdentifier == "tuttle.ramp") ||
                (stdIdentifier == "tuttle.seexpr") ) {
        s = QString::fromUtf8(PLUGIN_GROUP_IMAGE);
    } else if ( (stdIdentifier == "tuttle.bitdepth") ||
                (stdIdentifier == "tuttle.colorgradation") ||
                (stdIdentifier == "tuttle.colorspace") ||
                (stdIdentifier == "tuttle.colorsuppress") ||
                (stdIdentifier == "tuttle.colortransfer") ||
                (stdIdentifier == "tuttle.colortransform") ||
                (stdIdentifier == "tuttle.ctl") ||
                (stdIdentifier == "tuttle.invert") ||
                (stdIdentifier == "tuttle.lut") ||
                (stdIdentifier == "tuttle.normalize") ) {
        s = QString::fromUtf8(PLUGIN_GROUP_COLOR);
    } else if ( (stdIdentifier == "tuttle.ocio.colorspace") ||
                (stdIdentifier == "tuttle.ocio.lut") ) {
        out.push_back( QString::fromUtf8(PLUGIN_GROUP_COLOR) );
        s = QString::fromUtf8("OCIO");
    } else if ( (stdIdentifier == "tuttle.gamma") ||
                (stdIdentifier == "tuttle.mathoperator") ) {
        out.push_back( QString::fromUtf8(PLUGIN_GROUP_COLOR) );
        s = QString::fromUtf8("Math");
    } else if ( (stdIdentifier == "tuttle.channelshuffle") ) {
        s = QString::fromUtf8(PLUGIN_GROUP_CHANNEL);
    } else if ( (stdIdentifier == "tuttle.component") ||
                (stdIdentifier == "tuttle.fade") ||
                (stdIdentifier == "tuttle.merge") ) {
        s = QString::fromUtf8(PLUGIN_GROUP_MERGE);
    } else if ( (stdIdentifier == "tuttle.anisotropicdiffusion") ||
                (stdIdentifier == "tuttle.anisotropictensors") ||
                (stdIdentifier == "tuttle.blur") ||
                (stdIdentifier == "tuttle.convolution") ||
                (stdIdentifier == "tuttle.floodfill") ||
                (stdIdentifier == "tuttle.localmaxima") ||
                (stdIdentifier == "tuttle.nlmdenoiser") ||
                (stdIdentifier == "tuttle.sobel") ||
                (stdIdentifier == "tuttle.thinning") ) {
        s = QString::fromUtf8(PLUGIN_GROUP_FILTER);
    } else if ( (stdIdentifier == "tuttle.crop") ||
                (stdIdentifier == "tuttle.flip") ||
                (stdIdentifier == "tuttle.lensdistort") ||
                (stdIdentifier == "tuttle.move2d") ||
                (stdIdentifier == "tuttle.pinning") ||
                (stdIdentifier == "tuttle.pushpixel") ||
                (stdIdentifier == "tuttle.resize") ||
                (stdIdentifier == "tuttle.swscale") ||
                (stdIdentifier == "tuttle.warp") ) {
        s = QString::fromUtf8(PLUGIN_GROUP_TRANSFORM);
    } else if ( (stdIdentifier == "tuttle.timeshift") ) {
        s = QString::fromUtf8(PLUGIN_GROUP_TIME);
    } else if ( (stdIdentifier == "tuttle.text") ) {
        s = QString::fromUtf8(PLUGIN_GROUP_PAINT);
    } else if ( (stdIdentifier == "tuttle.basickeyer") ||
                (stdIdentifier == "tuttle.colorspacekeyer") ||
                (stdIdentifier == "tuttle.histogramkeyer") ||
                (stdIdentifier == "tuttle.idkeyer") ) {
        s = QString::fromUtf8(PLUGIN_GROUP_KEYER);
    } else if ( (stdIdentifier == "tuttle.colorCube") || // TuttleColorCubeViewer
                (stdIdentifier == "tuttle.colorcubeviewer") ||
                (stdIdentifier == "tuttle.diff") ||
                (stdIdentifier == "tuttle.dummy") ||
                (stdIdentifier == "tuttle.histogram") ||
                (stdIdentifier == "tuttle.imagestatistics") ) {
        s = QString::fromUtf8(PLUGIN_GROUP_OTHER);
    } else if ( (stdIdentifier == "tuttle.debugimageeffectapi") ) {
        out.push_back( QString::fromUtf8(PLUGIN_GROUP_OTHER) );
        s = QString::fromUtf8("Test");
    }

    // The following plugins are pretty much useless for use within Natron, keep them in the Tuttle group:

    /*
       (stdIdentifier == "tuttle.print") ||
       (stdIdentifier == "tuttle.viewer") ||
     */
    return out + s.split( QLatin1Char('/') );
} // ofxExtractAllPartsOfGrouping

std::vector<std::string>
AbstractOfxEffectInstance::makePluginGrouping(const std::string & pluginIdentifier,
                                              int versionMajor,
                                              int versionMinor,
                                              const std::string & pluginLabel,
                                              const std::string & grouping)
{
    //printf("%s,%s\n",pluginLabel.c_str(),grouping.c_str());
    QStringList list = ofxExtractAllPartsOfGrouping( QString::fromUtf8( pluginIdentifier.c_str() ), versionMajor, versionMinor, QString::fromUtf8( pluginLabel.c_str() ), QString::fromUtf8( grouping.c_str() ) );
    std::vector<std::string> ret(list.size());
    int i = 0;
    for (QStringList::iterator it = list.begin(); it!=list.end(); ++it, ++i) {
        ret[i] = it->trimmed().toStdString();
    }
    return ret;
}

std::string
AbstractOfxEffectInstance::makePluginLabel(const std::string & shortLabel,
                                           const std::string & label,
                                           const std::string & longLabel)
{
    std::string labelToUse = label;

    if ( labelToUse.empty() ) {
        labelToUse = shortLabel;
    }
    if ( labelToUse.empty() ) {
        labelToUse = longLabel;
    }

    return labelToUse;
}


void
OfxEffectInstance::onClipLabelChanged(int inputNb, const std::string& label)
{
    assert(inputNb >= 0 && inputNb < (int)_imp->common->clipsInfos.size());
    _imp->common->clipsInfos[inputNb].label = label;
    getNode()->setInputLabel(inputNb, label);
}

void
OfxEffectInstance::onClipHintChanged(int inputNb, const std::string& hint)
{
    assert(inputNb >= 0 && inputNb < (int)_imp->common->clipsInfos.size());
    _imp->common->clipsInfos[inputNb].hint = hint;
    getNode()->setInputHint(inputNb, hint);
}

void
OfxEffectInstance::onClipSecretChanged(int inputNb, bool isSecret)
{
    assert(inputNb >= 0 && inputNb < (int)_imp->common->clipsInfos.size());
    _imp->common->clipsInfos[inputNb].visible = !isSecret;
    getNode()->setInputVisible(inputNb, !isSecret);
}

std::string
OfxEffectInstance::getInputLabel(int inputNb) const
{
    assert(_imp->common->context != eContextNone);
    assert( inputNb >= 0 &&  inputNb < (int)_imp->common->clipsInfos.size() );
    if (_imp->common->context != eContextReader) {
        return _imp->common->clipsInfos[inputNb].clip->getShortLabel();
    } else {
        return NATRON_READER_INPUT_NAME;
    }
}

std::string
OfxEffectInstance::getInputHint(int inputNb) const
{
    assert(_imp->common->context != eContextNone);
    assert( inputNb >= 0 &&  inputNb < (int)_imp->common->clipsInfos.size() );
    if (_imp->common->context != eContextReader) {
        return _imp->common->clipsInfos[inputNb].clip->getHint();
    } else {
        return NATRON_READER_INPUT_NAME;
    }
}

OfxEffectInstance::MappedInputV
OfxEffectInstance::inputClipsCopyWithoutOutput() const
{
    assert(_imp->common->context != eContextNone);
    assert( effectInstance() );
    const std::vector<OFX::Host::ImageEffect::ClipDescriptor*> & clips = effectInstance()->getDescriptor().getClipsByOrder();
    MappedInputV copy;
    for (U32 i = 0; i < clips.size(); ++i) {
        assert(clips[i]);
        if ( !clips[i]->isOutput() ) {
            copy.push_back(clips[i]);
        }
    }

    return copy;
}

OfxClipInstance*
OfxEffectInstance::getClipCorrespondingToInput(int inputNo) const
{
    assert(_imp->common->context != eContextNone);
    assert( inputNo < (int)_imp->common->clipsInfos.size() );

    return _imp->common->clipsInfos[inputNo].clip;
}

int
OfxEffectInstance::getMaxInputCount() const
{
    assert(_imp->common->context != eContextNone);

    //const std::string & context = effectInstance()->getContext();
    return _imp->common->nbSourceClips;
}

bool
OfxEffectInstance::isInputOptional(int inputNb) const
{
    assert(_imp->common->context != eContextNone);
    assert( inputNb >= 0 && inputNb < (int)_imp->common->clipsInfos.size() );

    return _imp->common->clipsInfos[inputNb].optional;
}

bool
OfxEffectInstance::isInputMask(int inputNb) const
{
    assert(_imp->common->context != eContextNone);
    assert( inputNb >= 0 && inputNb < (int)_imp->common->clipsInfos.size() );

    return _imp->common->clipsInfos[inputNb].mask;
}


void
OfxEffectInstance::onInputChanged(int inputNo)
{
    assert(_imp->common->context != eContextNone);
    OfxClipInstance* clip = getClipCorrespondingToInput(inputNo);
    assert(clip);
    TimeValue time(getApp()->getTimeLine()->currentFrame());
    RenderScale s(1.);

    assert(_imp->common->effect);


    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
    EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxActionInstanceChanged, TimeValue(getApp()->getTimeLine()->currentFrame()), ViewIdx(0), RenderScale(1.)
#ifdef DEBUG
                                              , /*canSetValue*/ true
                                              , /*canBeCalledRecursively*/ true
#endif
                                              );


    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

    _imp->common->effect->beginInstanceChangedAction(kOfxChangeUserEdited);
    _imp->common->effect->clipInstanceChangedAction(clip->getName(), kOfxChangeUserEdited, time, s);
    _imp->common->effect->endInstanceChangedAction(kOfxChangeUserEdited);

}

/** @brief map a std::string to a context */
ContextEnum
OfxEffectInstance::mapToContextEnum(const std::string &s)
{
    if (s == kOfxImageEffectContextGenerator) {
        return eContextGenerator;
    }
    if (s == kOfxImageEffectContextFilter) {
        return eContextFilter;
    }
    if (s == kOfxImageEffectContextTransition) {
        return eContextTransition;
    }
    if (s == kOfxImageEffectContextPaint) {
        return eContextPaint;
    }
    if (s == kOfxImageEffectContextGeneral) {
        return eContextGeneral;
    }
    if (s == kOfxImageEffectContextRetimer) {
        return eContextRetimer;
    }
    if (s == kOfxImageEffectContextReader) {
        return eContextReader;
    }
    if (s == kOfxImageEffectContextWriter) {
        return eContextWriter;
    }
    if (s == kNatronOfxImageEffectContextTracker) {
        return eContextTracker;
    }
    qDebug() << "OfxEffectInstance::mapToContextEnum: Unknown image effect context '" << s.c_str() << "'";
    throw std::invalid_argument(s);
}

std::string
OfxEffectInstance::mapContextToString(ContextEnum ctx)
{
    switch (ctx) {
    case eContextGenerator:

        return kOfxImageEffectContextGenerator;
    case eContextFilter:

        return kOfxImageEffectContextFilter;
    case eContextTransition:

        return kOfxImageEffectContextTransition;
    case eContextPaint:

        return kOfxImageEffectContextPaint;
    case eContextGeneral:

        return kOfxImageEffectContextGeneral;
    case eContextRetimer:

        return kOfxImageEffectContextRetimer;
    case eContextReader:

        return kOfxImageEffectContextReader;
    case eContextWriter:

        return kOfxImageEffectContextWriter;
    case eContextTracker:

        return kNatronOfxImageEffectContextTracker;
    case eContextNone:
    default:
        break;
    }

    return std::string();
}


ActionRetCodeEnum
OfxEffectInstance::getTimeInvariantMetaDatas(NodeMetadata& metadata)
{
    if (!_imp->common->initialized || !_imp->common->effect) {
        return eActionStatusFailed;
    }
    assert(_imp->common->context != eContextNone);


    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
    if (tls->hasActionInStack(kOfxImageEffectActionGetClipPreferences)) {
        return eActionStatusReplyDefault;
    }
    EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxImageEffectActionGetClipPreferences, TimeValue(0), ViewIdx(0), RenderScale(1.)
#ifdef DEBUG
                                              , /*canSetValue*/ true
                                              , /*canBeCalledRecursively*/ true
#endif
                                              );

    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

    // It has been overriden and no data is actually set on the clip, everything will be set into the
    // metadata object
    ActionRetCodeEnum stat = _imp->common->effect->getClipPreferences_safe(metadata);
    return stat;
}


void
OfxEffectInstance::onMetadataChanged(const NodeMetadata& metadata)
{
    assert(_imp->common->context != eContextNone);
    TimeValue time(getApp()->getTimeLine()->currentFrame());
    RenderScale s(1.);

    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
    EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxActionInstanceChanged, TimeValue(getApp()->getTimeLine()->currentFrame()), ViewIdx(0), RenderScale(1.)
#ifdef DEBUG
                                              , /*canSetValue*/ true
                                              , /*canBeCalledRecursively*/ true
#endif
                                              );


    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
    
    assert(_imp->common->effect);
    _imp->common->effect->beginInstanceChangedAction(kOfxChangeUserEdited);
    _imp->common->effect->clipInstanceChangedAction(kOfxImageEffectOutputClipName, kOfxChangeUserEdited, time, s);
    _imp->common->effect->endInstanceChangedAction(kOfxChangeUserEdited);

    
    EffectInstance::onMetadataChanged(metadata);
}

ActionRetCodeEnum
OfxEffectInstance::getRegionOfDefinition(TimeValue time,
                                         const RenderScale & scale,
                                         ViewIdx view,
                                         RectD* rod)
{
    assert(_imp->common->context != eContextNone);
    if (!_imp->common->initialized) {
        return eActionStatusFailed;
    }

    assert(_imp->common->effect);


    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();

    if (tls->hasActionInStack(kOfxImageEffectActionGetRegionOfDefinition)) {
        return eActionStatusFailed;
    }
    EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxImageEffectActionGetRegionOfDefinition, time, view, scale
#ifdef DEBUG
                                              , /*canSetValue*/ false
                                              , /*canBeCalledRecursively*/ true
#endif
                                              );

    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

    OfxRectD ofxRod;
    OfxStatus stat = _imp->common->effect->getRegionOfDefinitionAction(time, scale, view, ofxRod);
    if (stat == kOfxStatFailed) {
        return eActionStatusFailed;
    }

    // If the rod is 1 pixel, determine if it was because one clip was unconnected or this is really a
    // 1 pixel large image
    if ( (ofxRod.x2 == 1.) && (ofxRod.y2 == 1.) && (ofxRod.x1 == 0.) && (ofxRod.y1 == 0.) ) {
        int maxInputs = getMaxInputCount();
        for (int i = 0; i < maxInputs; ++i) {
            OfxClipInstance* clip = getClipCorrespondingToInput(i);
            if ( clip && !clip->getConnected() && !clip->getIsOptional() && !clip->getIsMask() ) {
                ///this is a mandatory source clip and it is not connected, return statfailed
                return eActionStatusInputDisconnected;
            }
        }
    }

    RectD::ofxRectDToRectD(ofxRod, rod);

    return eActionStatusOK;

    // OFX::Host::ImageEffect::ClipInstance* clip = effectInstance()->getClip(kOfxImageEffectOutputClipName);
    //assert(clip);
    //double pa = clip->getAspectRatio();
} // getRegionOfDefinition

void
OfxEffectInstance::calcDefaultRegionOfDefinition(TimeValue time,
                                                 const RenderScale & scale,
                                                 ViewIdx view,
                                                 RectD *rod)
{
    assert(_imp->common->context != eContextNone);
    if (!_imp->common->initialized) {
        throw std::runtime_error("OfxEffectInstance not initialized");
    }

    OfxRectD ofxRod;


    assert(_imp->common->effect);

    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));


    // from http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxImageEffectActionGetRegionOfDefinition
    // generator context - defaults to the project window,
    // filter and paint contexts - defaults to the RoD of the 'Source' input clip at the given time,
    // transition context - defaults to the union of the RoDs of the 'SourceFrom' and 'SourceTo' input clips at the given time,
    // general context - defaults to the union of the RoDs of all the effect non optional input clips at the given time, if none exist, then it is the project window
    // retimer context - defaults to the union of the RoD of the 'Source' input clip at the frame directly preceding the value of the 'SourceTime' double parameter and the frame directly after it

    // the following ofxh function does the job

    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();

    EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxImageEffectActionGetRegionOfDefinition, time, view, scale
#ifdef DEBUG
                                              , /*canSetValue*/ false
                                              , /*canBeCalledRecursively*/ true
#endif
                                              );



    ofxRod = _imp->common->effect->calcDefaultRegionOfDefinition(time, scale, view);


    rod->x1 = ofxRod.x1;
    rod->x2 = ofxRod.x2;
    rod->y1 = ofxRod.y1;
    rod->y2 = ofxRod.y2;
}

static void
rectToOfxRectD(const RectD & b,
               OfxRectD *out)
{
    out->x1 = b.left();
    out->x2 = b.right();
    out->y1 = b.bottom();
    out->y2 = b.top();
}

ActionRetCodeEnum
OfxEffectInstance::getRegionsOfInterest(TimeValue time,
                                        const RenderScale & scale,
                                        const RectD & renderWindow, //!< the region to be rendered in the output image, in Canonical Coordinates
                                        ViewIdx view,
                                        RoIMap* ret)
{
    assert(_imp->common->context != eContextNone);
    std::map<OFX::Host::ImageEffect::ClipInstance*, OfxRectD> inputRois;
    if (!_imp->common->initialized) {
        return eActionStatusFailed;
    }

    assert(renderWindow.x2 >= renderWindow.x1 && renderWindow.y2 >= renderWindow.y1);


    OfxStatus stat;

    {
        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        if (tls->hasActionInStack(kOfxImageEffectActionGetRegionsOfInterest)) {
            return eActionStatusFailed;
        }
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxImageEffectActionGetRegionsOfInterest, time, view, scale
#ifdef DEBUG
                                                  , /*canSetValue*/ false
                                                  , /*canBeCalledRecursively*/ false
#endif
                                                  );


        OfxRectD roi;
        rectToOfxRectD(renderWindow, &roi);

        assert(_imp->common->effect);
        stat = _imp->common->effect->getRegionOfInterestAction( (OfxTime)time, scale, view,
                                                        roi, inputRois );
    }


    if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
        return eActionStatusFailed;
    }

    //Default behaviour already handled in getRegionOfInterestAction

    for (std::map<OFX::Host::ImageEffect::ClipInstance*, OfxRectD>::iterator it = inputRois.begin(); it != inputRois.end(); ++it) {
        OfxClipInstance* clip = dynamic_cast<OfxClipInstance*>(it->first);
        assert(clip);
        if (clip) {
            int inputNb = clip->getInputNb();
            RectD inputRoi; // input RoI in canonical coordinates
            inputRoi.x1 = it->second.x1;
            inputRoi.x2 = it->second.x2;
            inputRoi.y1 = it->second.y1;
            inputRoi.y2 = it->second.y2;

            if ( inputRoi.isNull() ) {
                continue;
            }

            ///The RoI might be infinite if the getRoI action of the plug-in doesn't do anything and the input effect has an
            ///infinite rod.
            ifInfiniteclipRectToProjectDefault(&inputRoi);
            //if (!inputRoi.isNull()) {
            ret->insert( std::make_pair(inputNb, inputRoi) );
            //}
        }
    }
    return eActionStatusOK;
} // getRegionsOfInterest

ActionRetCodeEnum
OfxEffectInstance::getFramesNeeded(TimeValue time,
                                   ViewIdx view,
                                   FramesNeededMap* results)
{
    assert(_imp->common->context != eContextNone);
    if (!_imp->common->initialized) {
        return eActionStatusFailed;
    }
    assert(_imp->common->effect);

    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
    if (tls->hasActionInStack(kOfxImageEffectActionGetFramesNeeded)) {
        return eActionStatusFailed;
    }
    EffectActionArgsSetter_RAII actionArgsTls(tls,kOfxImageEffectActionGetFramesNeeded, time, view, RenderScale(1.)
#ifdef DEBUG
                                              , /*canSetValue*/ false
                                              , /*canBeCalledRecursively*/ true
#endif
                                              );

    if ( isViewAware() ) {
        OFX::Host::ImageEffect::ViewsRangeMap inputRanges;
        {
            assert(_imp->common->effect);
            
            ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

            OfxStatus stat = _imp->common->effect->getFrameViewsNeeded( (OfxTime)time, view, inputRanges );
            if (stat == kOfxStatFailed) {
                return eActionStatusFailed;
            }
        }


        for (OFX::Host::ImageEffect::ViewsRangeMap::iterator it = inputRanges.begin(); it != inputRanges.end(); ++it) {
            OfxClipInstance* clip = dynamic_cast<OfxClipInstance*>(it->first);
            assert(clip);
            if (clip) {
                int inputNb = clip->getInputNb();
                if (inputNb != -1) {
                    // convert HostSupport's std::map<int, std::vector<OfxRangeD> > to  Natron's FrameRangesMap
                    FrameRangesMap frameRanges;
                    const std::map<int, std::vector<OfxRangeD> >& ofxRanges = it->second;
                    for (std::map<int, std::vector<OfxRangeD> >::const_iterator itr = ofxRanges.begin(); itr != ofxRanges.end(); ++itr) {
                        frameRanges[ViewIdx(itr->first)] = itr->second;
                    }
                    results->insert( std::make_pair(inputNb, frameRanges) );
                }
            }
        }

    } else {
        OFX::Host::ImageEffect::RangeMap inputRanges;
        {
            assert(_imp->common->effect);

            ///Take the preferences lock so that it cannot be modified throughout the action.
            OfxStatus stat = _imp->common->effect->getFrameNeededAction( (OfxTime)time, inputRanges );
            if (stat == kOfxStatFailed) {
                return eActionStatusFailed;
            }
        }

        for (OFX::Host::ImageEffect::RangeMap::iterator it = inputRanges.begin(); it != inputRanges.end(); ++it) {
            OfxClipInstance* clip = dynamic_cast<OfxClipInstance*>(it->first);
            assert(clip);
            if (clip) {
                int inputNb = clip->getInputNb();
                if (inputNb != -1) {
                    FrameRangesMap viewRangeMap;
                    viewRangeMap.insert( std::make_pair(view, it->second) );
                    results->insert( std::make_pair(inputNb, viewRangeMap) );
                }
            }
        }

    }

    //Default is already handled by HostSupport

    return eActionStatusOK;
} // OfxEffectInstance::getFramesNeeded

ActionRetCodeEnum
OfxEffectInstance::getFrameRange(double *first, double *last)
{
    assert(_imp->common->context != eContextNone);
    if (!_imp->common->initialized) {
        return eActionStatusFailed;
    }
    OfxRangeD range;
    // getTimeDomain should only be called on the 'general', 'reader' or 'generator' contexts.
    //  see http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxImageEffectActionGetTimeDomain"
    // Edit: Also add the 'writer' context as we need the getTimeDomain action to be able to find out the frame range to render.
    OfxStatus st = kOfxStatReplyDefault;
    if ( (_imp->common->context == eContextGeneral) ||
         ( _imp->common->context == eContextReader) ||
         ( _imp->common->context == eContextWriter) ||
         ( _imp->common->context == eContextGenerator) ) {
        assert(_imp->common->effect);



        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        if (tls->hasActionInStack(kOfxImageEffectActionGetTimeDomain)) {
            return eActionStatusFailed;
        }
        EffectActionArgsSetter_RAII actionArgsTls(tls,kOfxImageEffectActionGetTimeDomain, TimeValue(0), ViewIdx(0), RenderScale(1.)
#ifdef DEBUG
                                                  , /*canSetValue*/ false
                                                  , /*canBeCalledRecursively*/ true
#endif
                                                  );
        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

        st = _imp->common->effect->getTimeDomainAction(range);
    }
    if (st == kOfxStatOK) {
        *first = range.min;
        *last = range.max;
        return eActionStatusOK;
    } else if (st == kOfxStatReplyDefault) {
        return EffectInstance::getFrameRange(first, last);
    }
    return eActionStatusFailed;
} // getFrameRange

ActionRetCodeEnum
OfxEffectInstance::isIdentity(TimeValue time,
                              const RenderScale & scale,
                              const RectI & renderWindow,
                              ViewIdx view,
                              TimeValue* inputTime,
                              ViewIdx* inputView,
                              int* inputNb)
{
    *inputView = view;
    *inputNb = -1;
    *inputTime = time;

    if (!_imp->common->initialized) {
        return eActionStatusFailed;
    }

    assert(_imp->common->context != eContextNone);

    const std::string field = kOfxImageFieldNone; // TODO: support interlaced data
    std::string inputclip;

    OfxTime identityTimeOfx = time;
    OfxStatus stat;
    {

        OfxRectI ofxRoI;
        ofxRoI.x1 = renderWindow.left();
        ofxRoI.x2 = renderWindow.right();
        ofxRoI.y1 = renderWindow.bottom();
        ofxRoI.y2 = renderWindow.top();

        assert(_imp->common->effect);

        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();

        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxImageEffectActionIsIdentity, time, view, scale
#ifdef DEBUG
                                                  , /*canSetValue*/ false
                                                  , /*canBeCalledRecursively*/ true
#endif
                                                  );

        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

        stat = _imp->common->effect->isIdentityAction(identityTimeOfx, field, ofxRoI, scale, view, inputclip);
        assert(stat != kOfxStatErrBadHandle);
        if (stat == kOfxStatFailed || stat == kOfxStatErrBadHandle) {
            return eActionStatusFailed;
        } else if (stat == kOfxStatReplyDefault) {
            return eActionStatusOK;
        }
    }

    OFX::Host::ImageEffect::ClipInstance* clip = _imp->common->effect->getClip(inputclip);
    if (!clip) {
        // this is a plugin-side error, don't crash
        qDebug() << "Error in OfxEffectInstance::render(): kOfxImageEffectActionIsIdentity returned an unknown clip: " << inputclip.c_str();
        return eActionStatusFailed;
    }
    OfxClipInstance* natronClip = dynamic_cast<OfxClipInstance*>(clip);
    assert(natronClip);
    if (!natronClip) {
        // coverity[dead_error_line]
        qDebug() << "Error in OfxEffectInstance::render(): kOfxImageEffectActionIsIdentity returned an unknown clip: " << inputclip.c_str();
        return eActionStatusFailed;
    }
    if ( natronClip->isOutput() ) {
        *inputNb = -2;
    } else {
        *inputNb = natronClip->getInputNb();
    }
    *inputTime = TimeValue(identityTimeOfx);

    return eActionStatusOK;

} // isIdentity

class OfxGLContextEffectData
    : public EffectOpenGLContextData
{
    void* _dataHandle;

public:

    OfxGLContextEffectData(bool isGPUContext)
        : EffectOpenGLContextData(isGPUContext)
        , _dataHandle(0)
    {
    }

    void setDataHandle(void *dataHandle)
    {
        _dataHandle = dataHandle;
    }

    void* getDataHandle() const
    {
        return _dataHandle;
    }

    virtual ~OfxGLContextEffectData() {}
};

ActionRetCodeEnum
OfxEffectInstance::beginSequenceRender(double first,
                                       double last,
                                       double step,
                                       bool interactive,
                                       const RenderScale & scale,
                                       bool isSequentialRender,
                                       bool isRenderResponseToUserInteraction,
                                       bool draftMode,
                                       ViewIdx view,
                                       RenderBackendTypeEnum backendType,
                                       const EffectOpenGLContextDataPtr& glContextData)
{

    OfxStatus stat;
    {
        OfxGLContextEffectData* isOfxGLData = dynamic_cast<OfxGLContextEffectData*>( glContextData.get() );
        void* oglData = isOfxGLData ? isOfxGLData->getDataHandle() : 0;


        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        if (tls->hasActionInStack(kOfxImageEffectActionBeginSequenceRender)) {
            return eActionStatusFailed;
        }
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxImageEffectActionBeginSequenceRender, TimeValue(first), view, scale
#ifdef DEBUG
                                                  , /*canSetValue*/ false
                                                  , /*canBeCalledRecursively*/ false
#endif
                                                  );
        

        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
        
        stat = effectInstance()->beginRenderAction(first, last, step,
                                                   interactive, scale,
                                                   isSequentialRender, isRenderResponseToUserInteraction,
                                                   backendType == eRenderBackendTypeOpenGL, oglData, draftMode, view);
    }

    if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
        return eActionStatusFailed;
    }

    return eActionStatusOK;
}

ActionRetCodeEnum
OfxEffectInstance::endSequenceRender(double first,
                                     double last,
                                     double step,
                                     bool interactive,
                                     const RenderScale & scale,
                                     bool isSequentialRender,
                                     bool isRenderResponseToUserInteraction,
                                     bool draftMode,
                                     ViewIdx view,
                                     RenderBackendTypeEnum backendType,
                                     const EffectOpenGLContextDataPtr& glContextData)
{

    OfxStatus stat;
    {


        OfxGLContextEffectData* isOfxGLData = dynamic_cast<OfxGLContextEffectData*>( glContextData.get() );
        void* oglData = isOfxGLData ? isOfxGLData->getDataHandle() : 0;


        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        if (tls->hasActionInStack(kOfxImageEffectActionEndSequenceRender)) {
            return eActionStatusFailed;
        }
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxImageEffectActionEndSequenceRender, TimeValue(first), view, scale
#ifdef DEBUG
                                                  , /*canSetValue*/ false
                                                  , /*canBeCalledRecursively*/ false
#endif
                                                  );

        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

        stat = effectInstance()->endRenderAction(first, last, step,
                                                 interactive, scale,
                                                 isSequentialRender, isRenderResponseToUserInteraction,
                                                 backendType == eRenderBackendTypeOpenGL, oglData, draftMode, view);
    }

    if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
        return eActionStatusFailed;
    }

    return eActionStatusOK;
}

ActionRetCodeEnum
OfxEffectInstance::render(const RenderActionArgs& args)
{
    if (!_imp->common->initialized) {
        return eActionStatusFailed;
    }

    assert( !args.outputPlanes.empty() );

    OfxRectI ofxRoI;
    ofxRoI.x1 = args.roi.left();
    ofxRoI.x2 = args.roi.right();
    ofxRoI.y1 = args.roi.bottom();
    ofxRoI.y2 = args.roi.top();
    int viewsCount = getApp()->getProject()->getProjectViewsCount();
    OfxStatus stat;
    const std::string field = kOfxImageFieldNone; // TODO: support interlaced data
    std::list<std::string> ofxPlanes;

    std::map<ImagePlaneDesc, ImagePtr> outputPlanesMap;
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr > >::const_iterator it = args.outputPlanes.begin();
         it != args.outputPlanes.end(); ++it) {
        ofxPlanes.push_back(ImagePlaneDesc::mapPlaneToOFXPlaneString(it->first));
        outputPlanesMap[it->first] = it->second;
    }

    {
        assert(_imp->common->effect);

        OfxGLContextEffectData* isOfxGLData = dynamic_cast<OfxGLContextEffectData*>( args.glContextData.get() );
        void* oglData = isOfxGLData ? isOfxGLData->getDataHandle() : 0;

        TreeRenderPtr render = getCurrentRender();

        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();


        RenderActionArgsSetter_RAII actionArgsTls(tls, args.time, args.view, args.renderScale, args.roi, outputPlanesMap);
        
        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
        
        stat = _imp->common->effect->renderAction((OfxTime)args.time,
                                          field,
                                          ofxRoI,
                                          args.renderScale,
                                          render->isPlayback(),
                                          !render->isPlayback(),
                                          args.backendType == eRenderBackendTypeOpenGL,
                                          oglData,
                                          render->isDraftRender(),
                                          args.view,
                                          viewsCount,
                                          ofxPlanes );
    }

    if (stat != kOfxStatOK) {
        if ( !getNode()->hasPersistentMessage() ) {
            QString err;
            if (stat == kOfxStatErrImageFormat) {
                err = tr("Bad image format was supplied by %1.").arg( QString::fromUtf8(NATRON_APPLICATION_NAME) );
                setPersistentMessage( eMessageTypeError, err.toStdString() );
            } else if (stat == kOfxStatErrMemory) {
                err = tr("Out of memory!");
                setPersistentMessage( eMessageTypeError, err.toStdString() );
            } else {
                QString existingMessage;
                int type;
                getNode()->getPersistentMessage(&existingMessage, &type);
                if (existingMessage.isEmpty()) {
                    err = tr("Unknown failure reason.");
                }
            }

        }

        return eActionStatusFailed;
    } else {
        return eActionStatusOK;
    }
} // render

bool
OfxEffectInstance::supportsMultipleClipPARs() const
{
    return _imp->common->supportsMultipleClipPARs;
}

bool
OfxEffectInstance::supportsMultipleClipDepths() const
{
    return _imp->common->supportsMultipleClipDepths;
}

PluginOpenGLRenderSupport
OfxEffectInstance::getCurrentOpenGLSupport() const
{
    const std::string& str = effectInstance()->getProps().getStringProperty(kOfxImageEffectPropOpenGLRenderSupported);

    if (str == "false") {
        return ePluginOpenGLRenderSupportNone;
    } else if (str == "true") {
        return ePluginOpenGLRenderSupportYes;
    } else {
        assert(str == "needed");

        return ePluginOpenGLRenderSupportNeeded;
    }
}


const std::string &
OfxEffectInstance::getShortLabel() const
{
    return effectInstance()->getShortLabel();
}

void
OfxEffectInstance::initializeOverlayInteract()
{
    tryInitializeOverlayInteracts();
}

bool
OfxEffectInstance::canHandleRenderScaleForOverlays() const
{
    return _imp->common->overlaysCanHandleRenderScale;
}

void
OfxEffectInstance::drawOverlay(TimeValue time,
                               const RenderScale & renderScale,
                               ViewIdx view)
{
    if (!_imp->common->initialized) {
        return;
    }
    if (_imp->common->overlayInteract) {

        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxInteractActionDraw,  time, view, renderScale
#ifdef DEBUG
                                                  , /*canSetValue*/ false
                                                  , /*canBeCalledRecursively*/ false
#endif
                                                  );

        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
        
        _imp->common->overlayInteract->drawAction(time, renderScale, view, _imp->common->overlayInteract->hasColorPicker() ? &_imp->common->overlayInteract->getLastColorPickerColor() : /*colourPicker=*/0);
    }
}

void
OfxEffectInstance::setCurrentViewportForOverlays(OverlaySupport* viewport)
{
    if (_imp->common->overlayInteract) {
        _imp->common->overlayInteract->setCallingViewport(viewport);
    }
}

bool
OfxEffectInstance::onOverlayPenDown(TimeValue time,
                                    const RenderScale & renderScale,
                                    ViewIdx view,
                                    const QPointF & viewportPos,
                                    const QPointF & pos,
                                    double pressure,
                                    TimeValue /*timestamp*/,
                                    PenType /*pen*/)
{
    if (!_imp->common->initialized) {
        return false;
    }
    if (_imp->common->overlayInteract) {


        OfxPointD penPos;
        penPos.x = pos.x();
        penPos.y = pos.y();
        OfxPointI penPosViewport;
        penPosViewport.x = viewportPos.x();
        penPosViewport.y = viewportPos.y();

        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxInteractActionPenDown, time, view, renderScale
#ifdef DEBUG
                                                  , /*canSetValue*/ true
                                                  , /*canBeCalledRecursively*/ true
#endif
                                                  );

        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

        OfxStatus stat = _imp->common->overlayInteract->penDownAction(time, renderScale, view, _imp->common->overlayInteract->hasColorPicker() ? &_imp->common->overlayInteract->getLastColorPickerColor() : /*colourPicker=*/0, penPos, penPosViewport, pressure);

        if (stat == kOfxStatOK) {
            _imp->common->penDown = true;

            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayPenMotion(TimeValue time,
                                      const RenderScale & renderScale,
                                      ViewIdx view,
                                      const QPointF & viewportPos,
                                      const QPointF & pos,
                                      double pressure,
                                      TimeValue /*timestamp*/)
{
    if (!_imp->common->initialized) {
        return false;
    }
    if (_imp->common->overlayInteract) {


        OfxPointD penPos;
        penPos.x = pos.x();
        penPos.y = pos.y();
        OfxPointI penPosViewport;
        penPosViewport.x = viewportPos.x();
        penPosViewport.y = viewportPos.y();
        OfxStatus stat;


        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxInteractActionPenMotion, time, view, renderScale
#ifdef DEBUG
                                                  , /*canSetValue*/ true
                                                  , /*canBeCalledRecursively*/ true
#endif
                                                  );

        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

        stat = _imp->common->overlayInteract->penMotionAction(time, renderScale, view, _imp->common->overlayInteract->hasColorPicker() ? &_imp->common->overlayInteract->getLastColorPickerColor() : /*colourPicker=*/0, penPos, penPosViewport, pressure);

        if (stat == kOfxStatOK) {
            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayPenUp(TimeValue time,
                                  const RenderScale & renderScale,
                                  ViewIdx view,
                                  const QPointF & viewportPos,
                                  const QPointF & pos,
                                  double pressure,
                                  TimeValue /*timestamp*/)
{
    if (!_imp->common->initialized) {
        return false;
    }
    if (_imp->common->overlayInteract) {


        OfxPointD penPos;
        penPos.x = pos.x();
        penPos.y = pos.y();
        OfxPointI penPosViewport;
        penPosViewport.x = viewportPos.x();
        penPosViewport.y = viewportPos.y();

        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxInteractActionPenUp, time, view, renderScale
#ifdef DEBUG
                                                  , /*canSetValue*/ true
                                                  , /*canBeCalledRecursively*/ true
#endif
                                                  );

        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

        OfxStatus stat = _imp->common->overlayInteract->penUpAction(time, renderScale, view, _imp->common->overlayInteract->hasColorPicker() ? &_imp->common->overlayInteract->getLastColorPickerColor() : /*colourPicker=*/0, penPos, penPosViewport, pressure);
        if (stat == kOfxStatOK) {
            _imp->common->penDown = false;

            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayKeyDown(TimeValue time,
                                    const RenderScale & renderScale,
                                    ViewIdx view,
                                    Key key,
                                    KeyboardModifiers /*modifiers*/)
{
    if (!_imp->common->initialized) {
        return false;
    }
    if (_imp->common->overlayInteract) {

        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxInteractActionKeyDown, time, view, renderScale
#ifdef DEBUG
                                                  , /*canSetValue*/ true
                                                  , /*canBeCalledRecursively*/ true
#endif
                                                  );

        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

        QByteArray keyStr;
        OfxStatus stat = _imp->common->overlayInteract->keyDownAction( time, renderScale, view,_imp->common->overlayInteract->hasColorPicker() ? &_imp->common->overlayInteract->getLastColorPickerColor() : /*colourPicker=*/0, (int)key, keyStr.data() );

        if (stat == kOfxStatOK) {
            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayKeyUp(TimeValue time,
                                  const RenderScale & renderScale,
                                  ViewIdx view,
                                  Key key,
                                  KeyboardModifiers /* modifiers*/)
{
    if (!_imp->common->initialized) {
        return false;
    }
    if (_imp->common->overlayInteract) {


        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxInteractActionKeyUp, time, view, renderScale
#ifdef DEBUG
                                                  , /*canSetValue*/ true
                                                  , /*canBeCalledRecursively*/ true
#endif

                                                  );

        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
        
        QByteArray keyStr;
        OfxStatus stat = _imp->common->overlayInteract->keyUpAction( time, renderScale, view, _imp->common->overlayInteract->hasColorPicker() ? &_imp->common->overlayInteract->getLastColorPickerColor() : /*colourPicker=*/0, (int)key, keyStr.data() );

        if (stat == kOfxStatOK) {
            return true;
        }

    }

    return false;
}

bool
OfxEffectInstance::onOverlayKeyRepeat(TimeValue time,
                                      const RenderScale & renderScale,
                                      ViewIdx view,
                                      Key key,
                                      KeyboardModifiers /*modifiers*/)
{
    if (!_imp->common->initialized) {
        return false;
    }
    if (_imp->common->overlayInteract) {

        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxInteractActionKeyRepeat, time, view, renderScale
#ifdef DEBUG
                                                  , /*canSetValue*/ true
                                                  , /*canBeCalledRecursively*/ true
#endif

                                                  );
        
        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

        QByteArray keyStr;
        OfxStatus stat = _imp->common->overlayInteract->keyRepeatAction( time, renderScale, view, _imp->common->overlayInteract->hasColorPicker() ? &_imp->common->overlayInteract->getLastColorPickerColor() : /*colourPicker=*/0, (int)key, keyStr.data() );

        if (stat == kOfxStatOK) {
            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayFocusGained(TimeValue time,
                                        const RenderScale & renderScale,
                                        ViewIdx view)
{
    if (!_imp->common->initialized) {
        return false;
    }
    if (_imp->common->overlayInteract) {

        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxInteractActionGainFocus, time, view, renderScale
#ifdef DEBUG
                                                  , /*canSetValue*/ true
                                                  , /*canBeCalledRecursively*/ true
#endif

                                                  );

        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
        
        OfxStatus stat;
        stat = _imp->common->overlayInteract->gainFocusAction(time, renderScale, view, _imp->common->overlayInteract->hasColorPicker() ? &_imp->common->overlayInteract->getLastColorPickerColor() : /*colourPicker=*/0);
        if (stat == kOfxStatOK) {
            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayFocusLost(TimeValue time,
                                      const RenderScale & renderScale,
                                      ViewIdx view)
{
    if (!_imp->common->initialized) {
        return false;
    }
    if (_imp->common->overlayInteract) {


        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxInteractActionLoseFocus, time, view, renderScale
#ifdef DEBUG
                                                  , /*canSetValue*/ true
                                                  , /*canBeCalledRecursively*/ true
#endif

                                                  );

        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
        
        OfxStatus stat;
        stat = _imp->common->overlayInteract->loseFocusAction(time, renderScale, view, _imp->common->overlayInteract->hasColorPicker() ? &_imp->common->overlayInteract->getLastColorPickerColor() : /*colourPicker=*/0);
        if (stat == kOfxStatOK) {
            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::hasOverlay() const
{
    return _imp->common->overlayInteract != NULL;
}


std::string
OfxEffectInstance::natronValueChangedReasonToOfxValueChangedReason(ValueChangedReasonEnum reason)
{
    switch (reason) {
    case eValueChangedReasonUserEdited:
    case eValueChangedReasonRestoreDefault:

        return kOfxChangeUserEdited;
    case eValueChangedReasonPluginEdited:

        return kOfxChangePluginEdited;
    case eValueChangedReasonTimeChanged:

        return kOfxChangeTime;
    }
    assert(false);         // all Natron reasons should be processed
    return "";
}

class OfxUndoCommand : public UndoCommand
{
    KnobStringWPtr _textKnob;
    KnobBoolWPtr _stateKnob;
public:

    OfxUndoCommand(const KnobStringPtr& textKnob, const KnobBoolPtr &stateKnob)
    : _textKnob(textKnob)
    , _stateKnob(stateKnob)
    {
        setText(textKnob->getValue());
        stateKnob->blockValueChanges();
        stateKnob->setValue(true);
        stateKnob->unblockValueChanges();
    }

    virtual ~OfxUndoCommand()
    {

    }

    /**
     * @brief Called to redo the action
     **/
    virtual void redo() OVERRIDE FINAL
    {
        KnobBoolPtr state = _stateKnob.lock();
        bool currentValue = state->getValue();
        assert(!currentValue);
        state->setValue(true);
        if (currentValue) {
            state->evaluateValueChange(DimSpec::all(), TimeValue(0) /*time*/, ViewSetSpec::all(), eValueChangedReasonUserEdited);
        }
    }

    /**
     * @brief Called to undo the action
     **/
    virtual void undo() OVERRIDE FINAL
    {
        KnobBoolPtr state = _stateKnob.lock();
        bool currentValue = state->getValue();
        assert(currentValue);
        state->setValue(false, ViewSetSpec::all(), DimIdx(0), eValueChangedReasonUserEdited, 0);
        if (!currentValue) {
            state->evaluateValueChange(DimSpec::all(), TimeValue(0) /*time*/, ViewSetSpec::all(), eValueChangedReasonUserEdited);
        }
    }
    

};

bool
OfxEffectInstance::knobChanged(const KnobIPtr& k,
                               ValueChangedReasonEnum reason,
                               ViewSetSpec /*view*/,
                               TimeValue time)
{
    if (!_imp->common->initialized) {
        return false;
    }

    {
        // Handle cursor knob
        KnobStringPtr cursorKnob = _imp->common->cursorKnob.lock();
        if (k == cursorKnob) {
            CursorEnum c;
            std::string cursorStr = cursorKnob->getValue();
            if (OfxImageEffectInstance::ofxCursorToNatronCursor(cursorStr, &c)) {
                setCurrentCursor(c);
            } else {
                setCurrentCursor(QString::fromUtf8(cursorStr.c_str()));
            }
            return true;
        }
        KnobStringPtr undoRedoText = _imp->common->undoRedoTextKnob.lock();
        if (k == undoRedoText) {
            KnobBoolPtr undoRedoState = _imp->common->undoRedoStateKnob.lock();
            assert(undoRedoState);

            if (undoRedoState && reason == eValueChangedReasonPluginEdited) {
                UndoCommandPtr cmd(new OfxUndoCommand(undoRedoText, undoRedoState));
                pushUndoCommand(cmd);
                return true;
            }
        }

    }
    std::string ofxReason = natronValueChangedReasonToOfxValueChangedReason(reason);
    assert( !ofxReason.empty() ); // crashes when resetting to defaults
    RenderScale renderScale  = getNode()->getOverlayInteractRenderScale();
    
    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
    EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxActionInstanceChanged, time, ViewIdx(0), renderScale
#ifdef DEBUG
                                              , /*canSetValue*/ true
                                              , /*canBeCalledRecursively*/ true
#endif
                                              
                                              );
    


    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
    
    OfxStatus stat = kOfxStatOK;
    stat = effectInstance()->paramInstanceChangedAction(k->getOriginalName(), ofxReason, (OfxTime)time, renderScale);


    if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
        return false;
    }
    Q_UNUSED(stat);

    return true;
} // knobChanged

void
OfxEffectInstance::beginKnobsValuesChanged(ValueChangedReasonEnum reason)
{
    if (!_imp->common->initialized) {
        return;
    }

    
    
    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));


    ///This action as all the overlay interacts actions can trigger recursive actions, such as
    ///getClipPreferences() so we don't take the clips preferences lock for read here otherwise we would
    ///create a deadlock. This code then assumes that the instance changed action of the plug-in doesn't require
    ///the clip preferences to stay the same throughout the action.
    ignore_result( effectInstance()->beginInstanceChangedAction( natronValueChangedReasonToOfxValueChangedReason(reason) ) );
}

void
OfxEffectInstance::endKnobsValuesChanged(ValueChangedReasonEnum reason)
{
    if (!_imp->common->initialized) {
        return;
    }

    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));



    ///This action as all the overlay interacts actions can trigger recursive actions, such as
    ///getClipPreferences() so we don't take the clips preferences lock for read here otherwise we would
    ///create a deadlock. This code then assumes that the instance changed action of the plug-in doesn't require
    ///the clip preferences to stay the same throughout the action.
    ignore_result( effectInstance()->endInstanceChangedAction( natronValueChangedReasonToOfxValueChangedReason(reason) ) );
}

void
OfxEffectInstance::purgeCaches()
{

    if (!_imp->common->initialized) {
        return;
    }
    
    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
    
    // The kOfxActionPurgeCaches is an action that may be passed to a plug-in instance from time to time in low memory situations. Instances recieving this action should destroy any data structures they may have and release the associated memory, they can later reconstruct this from the effect's parameter set and associated information. http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxActionPurgeCaches
    OfxStatus stat;
    {
        assert(_imp->common->effect);

        ///Take the preferences lock so that it cannot be modified throughout the action.
        assert(_imp->common->effect);
        
        
        stat =  _imp->common->effect->purgeCachesAction();

        assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
    }
    // The kOfxActionSyncPrivateData action is called when a plugin should synchronise any private data structures to its parameter set. This generally occurs when an effect is about to be saved or copied, but it could occur in other situations as well. http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxActionSyncPrivateData

    {

        ///This action as all the overlay interacts actions can trigger recursive actions, such as
        ///getClipPreferences() so we don't take the clips preferences lock for read here otherwise we would
        ///create a deadlock. This code then assumes that the instance changed action of the plug-in doesn't require
        ///the clip preferences to stay the same throughout the action.
        stat =  _imp->common->effect->syncPrivateDataAction();
        assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
    }

    Q_UNUSED(stat);
}



bool
OfxEffectInstance::supportsRenderQuality() const
{
    return effectInstance()->supportsRenderQuality();
}

bool
OfxEffectInstance::supportsTiles() const
{
    // first, check the descriptor, then the instance
    if (!effectInstance()) {
        return false;
    }
    // This is a dynamic property since OFX 1.4, so get the prop from the instance, not the descriptor.
    // The descriptor may have it set to false for backward compatibility with hosts that do not support
    // this dynamic property.
    if ( !effectInstance()->supportsTiles() ) {
        return false;
    }

    OFX::Host::ImageEffect::ClipInstance* outputClip =  effectInstance()->getClip(kOfxImageEffectOutputClipName);

    return outputClip->supportsTiles();
}

void
OfxEffectInstance::onEnableOpenGLKnobValueChanged(bool activated)
{
    PluginPtr p = getNode()->getPlugin();
    PluginOpenGLRenderSupport support = (PluginOpenGLRenderSupport)p->getPropertyUnsafe<int>(kNatronPluginPropOpenGLSupport);
    if (support == ePluginOpenGLRenderSupportYes) {
        // The property may only change if the plug-in has the property set to yes on the descriptor
        if (activated) {
            effectInstance()->getProps().setStringProperty(kOfxImageEffectPropOpenGLRenderSupported, "true");
        } else {
            effectInstance()->getProps().setStringProperty(kOfxImageEffectPropOpenGLRenderSupported, "false");
        }
    }
}

bool
OfxEffectInstance::supportsMultiResolution() const
{
    // first, check the descriptor, then the instance
    if (!effectInstance()) {
        return false;
    }
    return effectInstance()->getDescriptor().supportsMultiResolution() && effectInstance()->supportsMultiResolution();
}

void
OfxEffectInstance::beginEditKnobs()
{
    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
    effectInstance()->beginInstanceEditAction();
}

void
OfxEffectInstance::syncPrivateData_other_thread()
{
    if ( getApp()->isShowingDialog() ) {
        /*
           We may enter a situation where a plug-in called EffectInstance::message to show a dialog
           and would block the main thread until the user would click OK but Qt would request a paintGL() on the viewer
           because of focus changes. This would end-up in the interact draw action being called whilst the message() function
           did not yet return and may in some plug-ins cause deadlocks (happens in all Genarts Sapphire plug-ins).
         */
        return;
    }
    Q_EMIT syncPrivateDataRequested();
}

void
OfxEffectInstance::onSyncPrivateDataRequested()
{
    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
    effectInstance()->syncPrivateDataAction();
}

void
OfxEffectInstance::addAcceptedComponents(int inputNb,
                                         std::bitset<4>* supported)
{
    OfxClipInstance* clip = 0;
    if (inputNb >= 0) {
        clip = getClipCorrespondingToInput(inputNb);
    } else {
        assert(inputNb == -1);
        clip = dynamic_cast<OfxClipInstance*>( effectInstance()->getClip(kOfxImageEffectOutputClipName) );
    }
    assert(clip);
    const std::vector<std::string> & supportedComps = clip->getSupportedComponents();
    for (U32 i = 0; i < supportedComps.size(); ++i) {
        try {
            ImagePlaneDesc plane, pairedPlane;
            ImagePlaneDesc::mapOFXComponentsTypeStringToPlanes(supportedComps[i], &plane, &pairedPlane);
            if (plane.getNumComponents() > 0) {
                (*supported)[plane.getNumComponents() - 1] = 1;
            }
        } catch (const std::runtime_error &e) {
            // ignore unsupported components
        }
    }
}

void
OfxEffectInstance::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    const OFX::Host::Property::Set & prop = effectInstance()->getPlugin()->getDescriptor().getParamSetProps();
    int dim = prop.getDimension(kOfxImageEffectPropSupportedPixelDepths);

    for (int i = 0; i < dim; ++i) {
        const std::string & depth = prop.getStringProperty(kOfxImageEffectPropSupportedPixelDepths, i);
        // ignore unsupported bitdepth
        ImageBitDepthEnum bitDepth = OfxClipInstance::ofxDepthToNatronDepth(depth, false);
        if (bitDepth != eImageBitDepthNone) {
            depths->push_back(bitDepth);
        }
    }
}

ActionRetCodeEnum
OfxEffectInstance::getLayersProducedAndNeeded(TimeValue time,
                                              ViewIdx view,
                                              std::map<int, std::list<ImagePlaneDesc> >* inputLayersNeeded,
                                              std::list<ImagePlaneDesc>* layersProduced,
                                              TimeValue* passThroughTime,
                                              ViewIdx* passThroughView,
                                              int* passThroughInputNb)
{


    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
    if (tls->hasActionInStack(kFnOfxImageEffectActionGetClipComponents)) {
        return eActionStatusFailed;
    }
    EffectActionArgsSetter_RAII actionArgsTls(tls,kFnOfxImageEffectActionGetClipComponents,  time, view, RenderScale(1.)
#ifdef DEBUG
                                              , /*canSetValue*/ false
                                              , /*canBeCalledRecursively*/ false
#endif
                                              );

    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

    OFX::Host::ImageEffect::ComponentsMap compMap;
    OFX::Host::ImageEffect::ClipInstance* ptClip = 0;
    OfxTime ptTime;
    int ptView_i;
    OfxStatus stat = effectInstance()->getClipComponentsAction( time, view, compMap, ptClip, ptTime, ptView_i );
    if (stat == kOfxStatFailed) {
        return eActionStatusFailed;
    }
    *passThroughInputNb = -1;
    if (ptClip) {
        OfxClipInstance* clip = dynamic_cast<OfxClipInstance*>(ptClip);
        if (clip) {
            *passThroughInputNb = clip->getInputNb();
        }
    }
    PassThroughEnum passThrough = isPassThroughForNonRenderedPlanes();
    if ( (passThrough == ePassThroughPassThroughNonRenderedPlanes) || (passThrough == ePassThroughRenderAllRequestedPlanes) ) {
        // If the plug-in is pass-through but did not indicate a clip, ensure we specify the main input at least
        *passThroughInputNb = getNode()->getPreferredInput();
    }
    *passThroughTime = TimeValue(ptTime);
    *passThroughView = ViewIdx(ptView_i);

    for (OFX::Host::ImageEffect::ComponentsMap::iterator it = compMap.begin(); it != compMap.end(); ++it) {
        OfxClipInstance* clip = dynamic_cast<OfxClipInstance*>(it->first);
        assert(clip);
        if (clip) {

            std::list<ImagePlaneDesc>* compsList = 0;
            if (clip->isOutput()) {
                compsList = layersProduced;
            } else {
                int index = clip->getInputNb();
                compsList = &(*inputLayersNeeded)[index];
            }


            for (std::list<std::string>::iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
                ImagePlaneDesc plane, pairedPlane;
                ImagePlaneDesc::mapOFXComponentsTypeStringToPlanes(*it2, &plane, &pairedPlane);
                if (plane.getNumComponents() > 0) {
                    compsList->push_back(plane);
                }
                if (pairedPlane.getNumComponents() > 0) {
                    compsList->push_back(pairedPlane);
                }

            }
        }
    }
    return eActionStatusOK;
} // getComponentsNeededAndProduced

bool
OfxEffectInstance::isMultiPlanar() const
{
    return _imp->common->multiplanar;
}

EffectInstance::PassThroughEnum
OfxEffectInstance::isPassThroughForNonRenderedPlanes() const
{
    OFX::Host::ImageEffect::Base::OfxPassThroughLevelEnum pt = effectInstance()->getPassThroughForNonRenderedPlanes();

    switch (pt) {
    case OFX::Host::ImageEffect::Base::ePassThroughLevelEnumBlockAllNonRenderedPlanes:

        return EffectInstance::ePassThroughBlockNonRenderedPlanes;
    case OFX::Host::ImageEffect::Base::ePassThroughLevelEnumPassThroughAllNonRenderedPlanes:

        return EffectInstance::ePassThroughPassThroughNonRenderedPlanes;
    case OFX::Host::ImageEffect::Base::ePassThroughLevelEnumRenderAllRequestedPlanes:

        return EffectInstance::ePassThroughRenderAllRequestedPlanes;
    }

    return EffectInstance::ePassThroughBlockNonRenderedPlanes;
}

bool
OfxEffectInstance::isViewAware() const
{
    return effectInstance() ? effectInstance()->isViewAware() : false;
}

EffectInstance::ViewInvarianceLevel
OfxEffectInstance::isViewInvariant() const
{
    int inv = effectInstance() ? effectInstance()->getViewInvariance() : 0;

    if (inv == 0) {
        return eViewInvarianceAllViewsVariant;
    } else if (inv == 1) {
        return eViewInvarianceOnlyPassThroughPlanesVariant;
    } else {
        assert(inv == 2);

        return eViewInvarianceAllViewsInvariant;
    }
}

SequentialPreferenceEnum
OfxEffectInstance::getSequentialPreference() const
{
    return _imp->common->sequentialPref;
}

bool
OfxEffectInstance::getCanTransform() const
{
    if (!effectInstance()) {
        return false;
    }
    return effectInstance()->canTransform();
}

bool
OfxEffectInstance::getCanDistort() const
{   //use OFX_EXTENSIONS_NATRON
    if (!effectInstance()) {
        return false;
    }
    return effectInstance()->canDistort();
}

bool
OfxEffectInstance::getInputCanReceiveTransform(int inputNb) const
{
    if (inputNb < 0 || inputNb >= (int)_imp->common->clipsInfos.size()) {
        assert(false);
        return false;
    }
    return _imp->common->clipsInfos[inputNb].canReceiveDistortion;

}

bool
OfxEffectInstance::getInputCanReceiveDistortion(int inputNb) const
{
    if (inputNb < 0 || inputNb >= (int)_imp->common->clipsInfos.size()) {
        assert(false);
        return false;
    }
    return _imp->common->clipsInfos[inputNb].canReceiveDistortion;
}


ActionRetCodeEnum
OfxEffectInstance::getDistortion(TimeValue time,
                                 const RenderScale & renderScale, //< the plug-in accepted scale
                                 ViewIdx view,
                                 DistortionFunction2D* distortion)
{
    const std::string field = kOfxImageFieldNone; // TODO: support interlaced data
    std::string clipName;
    double tmpTransform[9];
    OfxStatus stat;
    {

        EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
        if (tls->hasActionInStack(kFnOfxImageEffectActionGetTransform)) {
            return eActionStatusFailed;
        }
        EffectActionArgsSetter_RAII actionArgsTls(tls, kFnOfxImageEffectActionGetTransform, time, view, renderScale
#ifdef DEBUG
                                                  , /*canSetValue*/ false
                                                  , /*canBeCalledRecursively*/ true
#endif

                                                  );
        ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));
        
        try {
            if (effectInstance()->canDistort()) {
                stat = effectInstance()->getDistortionAction( (OfxTime)time, field, renderScale, view, clipName, tmpTransform, &distortion->func, &distortion->customData, &distortion->customDataSizeHintInBytes, &distortion->customDataFreeFunc );
            } else {
                stat = effectInstance()->getTransformAction( (OfxTime)time, field, renderScale, view, clipName, tmpTransform);
            }
        } catch (...) {
            return eActionStatusFailed;
        }

        if (stat == kOfxStatReplyDefault) {
            return eActionStatusReplyDefault;
        } else if (stat == kOfxStatFailed) {
            return eActionStatusFailed;
        }
    }


    assert(stat == kOfxStatOK);

    distortion->transformMatrix->a = tmpTransform[0]; distortion->transformMatrix->b = tmpTransform[1]; distortion->transformMatrix->c = tmpTransform[2];
    distortion->transformMatrix->d = tmpTransform[3]; distortion->transformMatrix->e = tmpTransform[4]; distortion->transformMatrix->f = tmpTransform[5];
    distortion->transformMatrix->g = tmpTransform[6]; distortion->transformMatrix->h = tmpTransform[7]; distortion->transformMatrix->i = tmpTransform[8];


    OFX::Host::ImageEffect::ClipInstance* clip = effectInstance()->getClip(clipName);
    assert(clip);
    OfxClipInstance* natronClip = dynamic_cast<OfxClipInstance*>(clip);
    if (!natronClip) {
        return eActionStatusFailed;
    }
    distortion->inputNbToDistort = natronClip->getInputNb();
   
    return eActionStatusOK;
}

bool
OfxEffectInstance::doesTemporalClipAccess() const
{
    // first, check the descriptor, then the instance
    return _imp->common->doesTemporalAccess;
}

bool
OfxEffectInstance::isHostChannelSelectorSupported(bool* defaultR,
                                                  bool* defaultG,
                                                  bool* defaultB,
                                                  bool* defaultA) const
{
    std::string defaultChannels = effectInstance()->getProps().getStringProperty(kNatronOfxImageEffectPropChannelSelector);

    if (defaultChannels == kOfxImageComponentNone) {
        return false;
    }
    if (defaultChannels == kOfxImageComponentRGBA) {
        *defaultR = *defaultG = *defaultB = *defaultA = true;
    } else if (defaultChannels == kOfxImageComponentRGB) {
        *defaultR = *defaultG = *defaultB = true;
        *defaultA = false;
    } else if (defaultChannels == kOfxImageComponentAlpha) {
        *defaultR = *defaultG = *defaultB = false;
        *defaultA = true;
    } else {
        qDebug() << getScriptName_mt_safe().c_str() << "Invalid value given to property" << kNatronOfxImageEffectPropChannelSelector << "defaulting to RGBA checked";
        *defaultR = *defaultG = *defaultB = *defaultA = true;
    }

    return true;
}

bool
OfxEffectInstance::isHostMaskingEnabled() const
{
    return effectInstance()->isHostMaskingEnabled();
}

bool
OfxEffectInstance::isHostMixingEnabled() const
{
    return effectInstance()->isHostMixingEnabled();
}

int
OfxEffectInstance::getClipInputNumber(const OfxClipInstance* clip) const
{
    for (std::size_t i = 0; i < _imp->common->clipsInfos.size(); ++i) {
        if (_imp->common->clipsInfos[i].clip == clip) {
            return (int)i;
        }
    }
    if (clip == _imp->common->outputClip) {
        return -1;
    }

    return 0;
}

void
OfxEffectInstance::onScriptNameChanged(const std::string& fullyQualifiedName)
{
    if (!_imp->common->effect) {
        return;
    }
    std::size_t foundLastDot = fullyQualifiedName.find_last_of('.');
    std::string scriptname, groupprefix, appID;
    if (foundLastDot == std::string::npos) {
        // No prefix
        scriptname = fullyQualifiedName;
    } else {
        if ( (foundLastDot + 1) < fullyQualifiedName.size() ) {
            scriptname = fullyQualifiedName.substr(foundLastDot + 1);
        }
        groupprefix = fullyQualifiedName.substr(0, foundLastDot);
    }
    appID = getApp()->getAppIDString();
    assert(_imp->common->effect);

    _imp->common->effect->getProps().setStringProperty(kNatronOfxImageEffectPropProjectId, appID);
    _imp->common->effect->getProps().setStringProperty(kNatronOfxImageEffectPropGroupId, groupprefix);
    _imp->common->effect->getProps().setStringProperty(kNatronOfxImageEffectPropInstanceId, scriptname);
}

bool
OfxEffectInstance::supportsConcurrentOpenGLRenders() const
{
    // By default OpenFX OpenGL render suite does not support concurrent OpenGL renders.
    QMutexLocker k(&_imp->common->supportsConcurrentGLRendersMutex);

    return _imp->common->supportsConcurrentGLRenders;
}

ActionRetCodeEnum
OfxEffectInstance::attachOpenGLContext(TimeValue time, ViewIdx view, const RenderScale& scale, const OSGLContextPtr& glContext, EffectOpenGLContextDataPtr* data)
{


    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
    if (tls->hasActionInStack(kOfxActionOpenGLContextAttached)) {
        return eActionStatusFailed;
    }
    EffectActionArgsSetter_RAII actionArgsTls(tls,kOfxActionOpenGLContextAttached, time, view, scale
#ifdef DEBUG
                                              , /*canSetValue*/ false
                                              , /*canBeCalledRecursively*/ false
#endif
                                              );

    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

    boost::shared_ptr<OfxGLContextEffectData> ofxData( new OfxGLContextEffectData(glContext->isGPUContext()) );

    *data = ofxData;
    void* ofxGLData = 0;
    OfxStatus stat = effectInstance()->contextAttachedAction(ofxGLData);

    // If the plug-in use the Natron property kNatronOfxImageEffectPropOpenGLContextData, that means it can handle
    // concurrent OpenGL renders.
    if (ofxGLData) {
        ofxData->setDataHandle(ofxGLData);
        QMutexLocker k(&_imp->common->supportsConcurrentGLRendersMutex);
        if (!_imp->common->supportsConcurrentGLRenders) {
            _imp->common->supportsConcurrentGLRenders = true;
        }
    }
    if (stat == kOfxStatFailed) {
        return eActionStatusFailed;
    } else if (stat == kOfxStatErrMemory) {
        return eActionStatusOutOfMemory;
    } else if (stat == kOfxStatReplyDefault) {
        return eActionStatusReplyDefault;
    } else {
        return eActionStatusOK;
    }
}

ActionRetCodeEnum
OfxEffectInstance::dettachOpenGLContext( const OSGLContextPtr& /*glContext*/, const EffectOpenGLContextDataPtr& data)
{


    EffectInstanceTLSDataPtr tls = _imp->common->tlsData->getOrCreateTLSData();
    if (tls->hasActionInStack(kOfxActionOpenGLContextDetached)) {
        return eActionStatusFailed;
    }
    EffectActionArgsSetter_RAII actionArgsTls(tls, kOfxActionOpenGLContextDetached, TimeValue(0), ViewIdx(0), RenderScale(1.)
#ifdef DEBUG
                                              , /*canSetValue*/ false
                                              , /*canBeCalledRecursively*/ false
#endif
                                              );

    
    ThreadIsActionCaller_RAII actionCaller(toOfxEffectInstance(shared_from_this()));

    OfxGLContextEffectData* isOfxData = dynamic_cast<OfxGLContextEffectData*>( data.get() );
    void* ofxGLData = isOfxData ? isOfxData->getDataHandle() : 0;
    OfxStatus stat = effectInstance()->contextDetachedAction(ofxGLData);

    if (isOfxData) {
        // the context data can not be used anymore, reset it.
        isOfxData->setDataHandle(NULL);
    }
    if (stat == kOfxStatFailed) {
        return eActionStatusFailed;
    } else if (stat == kOfxStatErrMemory) {
        return eActionStatusOutOfMemory;
    } else if (stat == kOfxStatReplyDefault) {
        return eActionStatusReplyDefault;
    } else {
        return eActionStatusOK;
    }
}

void
OfxEffectInstance::onInteractViewportSelectionCleared()
{
    KnobIntPtr k = _imp->common->selectionRectangleStateKnob.lock();
    if (!k) {
        return;
    }
    double propV[4] = {0, 0, 0, 0};
    effectInstance()->getProps().setDoublePropertyN(kNatronOfxImageEffectSelectionRectangle, propV, 4);

    k->setValue(0);
}


void
OfxEffectInstance::onInteractViewportSelectionUpdated(const RectD& rectangle, bool onRelease)
{
    KnobIntPtr k = _imp->common->selectionRectangleStateKnob.lock();
    if (!k) {
        return;
    }
    double propV[4] = {rectangle.x1, rectangle.y1, rectangle.x2, rectangle.y2};
    effectInstance()->getProps().setDoublePropertyN(kNatronOfxImageEffectSelectionRectangle, propV, 4);
    k->setValue(onRelease ? 2 : 1);
}


NATRON_NAMESPACE_EXIT;

NATRON_NAMESPACE_USING;
#include "moc_OfxEffectInstance.cpp"

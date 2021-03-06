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

#include "RotoPaint.h"
#include "RotoPaintPrivate.h"

#include <sstream> // stringstream
#include <cassert>
#include <stdexcept>

#include "Serialization/BezierSerialization.h"
#include "Serialization/RotoStrokeItemSerialization.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/NodeMetadata.h"
#include "Engine/MergingEnum.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/KnobTypes.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/KnobTypes.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/KnobItemsTableUndoCommand.h"
#include "Engine/RotoLayer.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

#include "Global/GLIncludes.h"
#include "Global/GlobalDefines.h"

#define kFilterImpulse "Impulse"
#define kFilterImpulseHint "(nearest neighbor / box) Use original values"
#define kFilterBilinear "Bilinear"
#define kFilterBilinearHint "(tent / triangle) Bilinear interpolation between original values"
#define kFilterCubic "Cubic"
#define kFilterCubicHint "(cubic spline) Some smoothing"
#define kFilterKeys "Keys"
#define kFilterKeysHint "(Catmull-Rom / Hermite spline) Some smoothing, plus minor sharpening (*)"
#define kFilterSimon "Simon"
#define kFilterSimonHint "Some smoothing, plus medium sharpening (*)"
#define kFilterRifman "Rifman"
#define kFilterRifmanHint "Some smoothing, plus significant sharpening (*)"
#define kFilterMitchell "Mitchell"
#define kFilterMitchellHint "Some smoothing, plus blurring to hide pixelation (*+)"
#define kFilterParzen "Parzen"
#define kFilterParzenHint "(cubic B-spline) Greatest smoothing of all filters (+)"
#define kFilterNotch "Notch"
#define kFilterNotchHint "Flat smoothing (which tends to hide moire' patterns) (+)"

//KnobPagePtr generalPage = AppManager::checkIfKnobExistsWithNameOrCreate<KnobPage>(effect, "generalPage", tr("General"));

#define kRotoPaintGeneralPageParam "generalPage"
#define kRotoPaintGeneralPageParamLabel "General"

#define kRotoPaintShapePageParam "shapePage"
#define kRotoPaintShapePageParamLabel "Shape"

#define kRotoPaintStrokePageParam "strokePage"
#define kRotoPaintStrokePageParamLabel "Stroke"

#define kRotoPaintClonePageParam "clonePage"
#define kRotoPaintClonePageParamLabel "Clone"

#define kRotoPaintTransformPageParam "transformPage"
#define kRotoPaintTransformPageParamLabel "Transform"

#define kRotoPaintMotionBlurPageParam "motionBlurPage"
#define kRotoPaintMotionBlurPageParamLabel "Motion-Blur"

#define ROTO_DEFAULT_OPACITY 1.
#define ROTO_DEFAULT_FEATHER 1.5
#define ROTO_DEFAULT_FEATHERFALLOFF 1.


#define ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX 5

NATRON_NAMESPACE_ENTER;

static void addPluginShortcuts(const PluginPtr& plugin)
{


    // Viewer buttons
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamAutoKeyingEnabled, kRotoUIParamAutoKeyingEnabledLabel, kRotoUIParamAutoKeyingEnabledHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamFeatherLinkEnabled, kRotoUIParamFeatherLinkEnabledLabel, kRotoUIParamFeatherLinkEnabledHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamDisplayFeather, kRotoUIParamDisplayFeatherLabel, kRotoUIParamDisplayFeatherHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamStickySelectionEnabled, kRotoUIParamStickySelectionEnabledLabel, kRotoUIParamStickySelectionEnabledHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamStickyBbox, kRotoUIParamStickyBboxLabel, kRotoUIParamStickyBboxHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRippleEdit, kRotoUIParamRippleEditLabel, kRotoUIParamRippleEditHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamAddKeyFrame, kRotoUIParamAddKeyFrameLabel, kRotoUIParamAddKeyFrameHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRemoveKeyframe, kRotoUIParamRemoveKeyframeLabel, kRotoUIParamRemoveKeyframeHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamShowTransform, kRotoUIParamShowTransformLabel, kRotoUIParamShowTransformHint, Key_T) );

    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamPressureOpacity, kRotoUIParamPressureOpacityLabel, kRotoUIParamPressureOpacityHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamPressureSize, kRotoUIParamPressureSizeLabel, kRotoUIParamPressureSizeHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamPressureHardness, kRotoUIParamPressureHardnessLabel, kRotoUIParamPressureHardnessHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamBuildUp, kRotoUIParamBuildUpLabel, kRotoUIParamBuildUpHint) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamResetCloneOffset, kRotoUIParamResetCloneOffsetLabel, kRotoUIParamResetCloneOffsetHint) );

    // Toolbuttons
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamSelectionToolButton, kRotoUIParamSelectionToolButtonLabel, "", Key_Q) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamEditPointsToolButton, kRotoUIParamEditPointsToolButtonLabel, "", Key_D) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamBezierEditionToolButton, kRotoUIParamBezierEditionToolButtonLabel, "", Key_V) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamPaintBrushToolButton, kRotoUIParamPaintBrushToolButtonLabel, "", Key_N) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamCloneBrushToolButton, kRotoUIParamCloneBrushToolButtonLabel, "", Key_C) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamEffectBrushToolButton, kRotoUIParamEffectBrushToolButtonLabel, "", Key_X) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamMergeBrushToolButton, kRotoUIParamMergeBrushToolButtonLabel, "", Key_E) );

    // Right click actions
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRightClickMenuActionRemoveItems, kRotoUIParamRightClickMenuActionRemoveItemsLabel, "", Key_BackSpace) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRightClickMenuActionCuspItems, kRotoUIParamRightClickMenuActionCuspItemsLabel, "", Key_Z, eKeyboardModifierShift) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRightClickMenuActionSmoothItems, kRotoUIParamRightClickMenuActionSmoothItemsLabel, "", Key_Z) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRightClickMenuActionRemoveItemsFeather, kRotoUIParamRightClickMenuActionRemoveItemsFeatherLabel, "", Key_E, eKeyboardModifierShift) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRightClickMenuActionNudgeLeft, kRotoUIParamRightClickMenuActionNudgeLeftLabel, "", Key_4) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRightClickMenuActionNudgeBottom, kRotoUIParamRightClickMenuActionNudgeBottomLabel, "", Key_2) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRightClickMenuActionNudgeTop, kRotoUIParamRightClickMenuActionNudgeTopLabel, "", Key_8) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRightClickMenuActionNudgeRight, kRotoUIParamRightClickMenuActionNudgeRightLabel, "", Key_6) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRightClickMenuActionSelectAll, kRotoUIParamRightClickMenuActionSelectAllLabel, "", Key_A, eKeyboardModifierControl) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRightClickMenuActionOpenClose, kRotoUIParamRightClickMenuActionOpenCloseLabel, "", Key_Return) );
    plugin->addActionShortcut( PluginActionShortcut(kRotoUIParamRightClickMenuActionLockShapes, kRotoUIParamRightClickMenuActionLockShapesLabel, "", Key_L, eKeyboardModifierShift) );

} // addPluginShortcuts

PluginPtr
RotoPaint::createPlugin()
{
    std::vector<std::string> grouping;
    grouping.push_back(PLUGIN_GROUP_PAINT);
    PluginPtr ret = Plugin::create((void*)RotoPaint::create, (void*)RotoPaint::createRenderClone, PLUGINID_NATRON_ROTOPAINT, "RotoPaint", 1, 0, grouping);

    QString desc = tr("RotoPaint is a vector based free-hand drawing node that helps for tasks such as rotoscoping, matting, etc...");
    ret->setProperty<std::string>(kNatronPluginPropDescription, desc.toStdString());
    ret->setProperty<int>(kNatronPluginPropRenderSafety, (int)eRenderSafetyFullySafe);
    ret->setProperty<std::string>(kNatronPluginPropIconFilePath, "Images/GroupingIcons/Set2/paint_grouping_2.png");
    ret->setProperty<int>(kNatronPluginPropShortcut, (int)Key_P);
    addPluginShortcuts(ret);
    return ret;
}

PluginPtr
RotoNode::createPlugin()
{
    std::vector<std::string> grouping;
    grouping.push_back(PLUGIN_GROUP_PAINT);
    PluginPtr ret = Plugin::create((void*)RotoNode::create, (void*)RotoNode::createRenderClone, PLUGINID_NATRON_ROTO, "Roto", 1, 0, grouping);

    QString desc = tr("Create masks and shapes.");
    ret->setProperty<std::string>(kNatronPluginPropDescription, desc.toStdString());
    ret->setProperty<int>(kNatronPluginPropRenderSafety, (int)eRenderSafetyFullySafe);
    ret->setProperty<std::string>(kNatronPluginPropIconFilePath, "Images/rotoNodeIcon.png");
    ret->setProperty<int>(kNatronPluginPropShortcut, (int)Key_O);
    addPluginShortcuts(ret);
    return ret;
}

PluginPtr
LayeredCompNode::createPlugin()
{
    std::vector<std::string> grouping;
    grouping.push_back(PLUGIN_GROUP_MERGE);
    PluginPtr ret = Plugin::create((void*)LayeredCompNode::create, (void*)LayeredCompNode::createRenderClone, PLUGINID_NATRON_LAYEREDCOMP, "LayeredComp", 1, 0, grouping);

    QString desc = tr("A node that emulates a layered composition.\n"
                      "Each item in the table is a layer that is blended with previous layers.\n"
                      "For each item you may select the node name that should be used as source "
                      "and optionnally the node name that should be used as a mask. These nodes "
                      "must be connected to the Source inputs and Mask inputs of the LayeredComp node itself.");
    ret->setProperty<std::string>(kNatronPluginPropDescription, desc.toStdString());
    ret->setProperty<int>(kNatronPluginPropRenderSafety, (int)eRenderSafetyFullySafe);
    ret->setProperty<std::string>(kNatronPluginPropIconFilePath, std::string("Images/") + std::string(PLUGIN_GROUP_MERGE_ICON_PATH));
    return ret;
}

RotoPaint::RotoPaint(const NodePtr& node,
                     RotoPaintTypeEnum type)
    : NodeGroup(node)
    , _imp( new RotoPaintPrivate(this, type) )
{
}


RotoPaint::~RotoPaint()
{
}

bool
RotoPaint::isSubGraphPersistent() const
{
    return false;
}

bool
RotoPaint::isSubGraphUserVisible() const
{
#ifdef ROTO_PAINT_NODE_GRAPH_VISIBLE
    return true;
#else
    return false;
#endif
}

RotoPaint::RotoPaintTypeEnum
RotoPaint::getRotoPaintNodeType() const
{
    return _imp->nodeType;
}

bool
RotoPaint::isHostMaskingEnabled() const
{
    return _imp->nodeType != eRotoPaintTypeComp;
}

bool
RotoPaint::isHostMixingEnabled() const
{
    return _imp->nodeType != eRotoPaintTypeComp;
}

bool
RotoPaint::getCreateChannelSelectorKnob() const
{
    return false;
}

bool
RotoPaint::isHostChannelSelectorSupported(bool* defaultR,
                                          bool* defaultG,
                                          bool* defaultB,
                                          bool* defaultA) const
{
    //Use our own selectors, we don't want Natron to copy back channels
    *defaultR = true;
    *defaultG = true;
    *defaultB = true;
    *defaultA = true;

    return false;
}

bool
RotoNode::isHostChannelSelectorSupported(bool* defaultR,
                                         bool* defaultG,
                                         bool* defaultB,
                                         bool* defaultA) const
{
    *defaultR = false;
    *defaultG = false;
    *defaultB = false;
    *defaultA = true;

    return false;
}

NodePtr
RotoPaint::getPremultNode() const
{
    return _imp->premultNode.lock();
}

NodePtr
RotoPaint::getMetadataFixerNode() const
{
    return _imp->premultFixerNode.lock();
}

NodePtr
RotoPaint::getInternalInputNode(int index) const
{
    if (index < 0 || index >= (int)_imp->inputNodes.size()) {
        return NodePtr();
    }
    return _imp->inputNodes[index].lock();
}

bool
RotoPaint::getDefaultInput(bool connected, int* inputIndex) const
{
    EffectInstancePtr input0 = getInput(0);
    if (!connected) {
        if (!input0) {
            *inputIndex = 0;
            return true;
        }
    } else {
        if (input0) {
            *inputIndex = 0;
            return true;
        }
    }
    return false;
}

void
RotoPaint::getEnabledChannelKnobs(KnobBoolPtr* r,KnobBoolPtr* g, KnobBoolPtr* b, KnobBoolPtr *a) const
{
    *r = _imp->enabledKnobs[0].lock();
    *g = _imp->enabledKnobs[1].lock();
    *b = _imp->enabledKnobs[2].lock();
    *a = _imp->enabledKnobs[3].lock();
}

void
RotoPaint::initLifeTimeKnobs(const KnobPagePtr& generalPage)
{
    EffectInstancePtr effect = shared_from_this();
    RotoPaintItemLifeTimeTypeEnum defaultLifeTime = _imp->nodeType == eRotoPaintTypeRotoPaint ? eRotoPaintItemLifeTimeTypeSingle : eRotoPaintItemLifeTimeTypeAll;
    {
        KnobChoicePtr param = createKnob<KnobChoice>(kRotoDrawableItemLifeTimeParam);
        param->setLabel(tr(kRotoDrawableItemLifeTimeParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemLifeTimeParamHint) );
        param->setAddNewLine(false);
        param->setAnimationEnabled(false);
        {
            std::vector<ChoiceOption> choices;
            assert(choices.size() == eRotoPaintItemLifeTimeTypeAll);
            choices.push_back(ChoiceOption(kRotoDrawableItemLifeTimeAll, "",tr(kRotoDrawableItemLifeTimeAllHelp).toStdString() ));
            assert(choices.size() == eRotoPaintItemLifeTimeTypeSingle);
            choices.push_back(ChoiceOption(kRotoDrawableItemLifeTimeSingle, "", tr(kRotoDrawableItemLifeTimeSingleHelp).toStdString()));
            assert(choices.size() == eRotoPaintItemLifeTimeTypeFromStart);
            choices.push_back(ChoiceOption(kRotoDrawableItemLifeTimeFromStart, "", tr(kRotoDrawableItemLifeTimeFromStartHelp).toStdString()));
            assert(choices.size() == eRotoPaintItemLifeTimeTypeToEnd);
            choices.push_back(ChoiceOption(kRotoDrawableItemLifeTimeToEnd, "" ,tr(kRotoDrawableItemLifeTimeToEndHelp).toStdString()));
            assert(choices.size() == eRotoPaintItemLifeTimeTypeCustom);
            choices.push_back(ChoiceOption(kRotoDrawableItemLifeTimeCustom, "", tr(kRotoDrawableItemLifeTimeCustomHelp).toStdString()));
            param->populateChoices(choices);
        }
        // Default to single frame lifetime, otherwise default to
        param->setDefaultValue(defaultLifeTime, DimSpec(0));
        _imp->knobsTable->addPerItemKnobMaster(param);
        generalPage->addKnob(param);
        _imp->lifeTimeKnob = param;
    }

    {
        KnobIntPtr param = createKnob<KnobInt>(kRotoDrawableItemLifeTimeFrameParam);
        param->setLabel(tr(kRotoDrawableItemLifeTimeFrameParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemLifeTimeFrameParamHint) );
        param->setSecret(defaultLifeTime != eRotoPaintItemLifeTimeTypeFromStart && defaultLifeTime != eRotoPaintItemLifeTimeTypeToEnd);
        param->setAddNewLine(false);
        param->setAnimationEnabled(false);
        _imp->knobsTable->addPerItemKnobMaster(param);
        generalPage->addKnob(param);
        _imp->lifeTimeFrameKnob = param;
    }

    {
        KnobBoolPtr param = createKnob<KnobBool>(kRotoLifeTimeCustomRangeParam);
        param->setHintToolTip( tr(kRotoLifeTimeCustomRangeParamHint) );
        param->setLabel(tr(kRotoLifeTimeCustomRangeParamLabel));
        param->setAddNewLine(true);
        param->setAnimationEnabled(true);
        param->setSecret(defaultLifeTime != eRotoPaintItemLifeTimeTypeCustom);
        param->setDefaultValue(true);
        _imp->knobsTable->addPerItemKnobMaster(param);
        generalPage->addKnob(param);
        _imp->customRangeKnob = param;
    }

} // initLifeTimeKnobs

void
RotoPaint::initGeneralPageKnobs()
{
    EffectInstancePtr effect = shared_from_this();

    KnobPagePtr generalPage = getOrCreateKnob<KnobPage>(kRotoPaintGeneralPageParam);
    generalPage->setLabel(tr(kRotoPaintGeneralPageParam));

    if (_imp->nodeType != eRotoPaintTypeComp) {
        {
            KnobDoublePtr param = createKnob<KnobDouble>(kRotoOpacityParam);
            param->setLabel(tr(kRotoOpacityParamLabel));
            param->setHintToolTip( tr(kRotoOpacityHint) );
            param->setRange(0., 1.);
            param->setDisplayRange(0., 1.);
            param->setDefaultValue(ROTO_DEFAULT_OPACITY, DimSpec(0));
            _imp->knobsTable->addPerItemKnobMaster(param);
            generalPage->addKnob(param);
        }

        {
            KnobColorPtr param = createKnob<KnobColor>(kRotoColorParam, 4);
            param->setLabel(tr(kRotoColorParamLabel));
            param->setHintToolTip( tr(kRotoColorHint) );
            std::vector<double> def(4, 1.);
            param->setDefaultValues(def, DimIdx(0));
            param->setDisplayRange(0., 1.);
            _imp->knobsTable->addPerItemKnobMaster(param);
            generalPage->addKnob(param);
        }
    }
    initLifeTimeKnobs(generalPage);

  
    {
        KnobButtonPtr param = createKnob<KnobButton>(kRotoAddGroupParam);
        param->setLabel(tr(kRotoAddGroupParamLabel));
        param->setHintToolTip( tr(kRotoAddGroupParamHint) );
        param->setAddNewLine(false);
        _imp->addGroupButtonKnob = param;
        generalPage->addKnob(param);
    }

    {
        KnobButtonPtr param = createKnob<KnobButton>(kRotoRemoveItemParam);
        param->setLabel(tr(kRotoRemoveItemParamLabel));
        param->setHintToolTip( tr(kRotoRemoveItemParamHint) );
        _imp->removeItemButtonKnob = param;
        generalPage->addKnob(param);
    }
} // initGeneralPageKnobs

void
RotoPaint::initShapePageKnobs()
{
    EffectInstancePtr effect = shared_from_this();

    KnobPagePtr shapePage = getOrCreateKnob<KnobPage>(kRotoPaintShapePageParam);
    shapePage->setLabel(tr(kRotoPaintShapePageParamLabel));

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoFeatherParam);
        param->setLabel(tr(kRotoFeatherParamLabel));
        param->setHintToolTip( tr(kRotoFeatherHint) );
        param->setRange(0, std::numeric_limits<double>::infinity());
        param->setDisplayRange(0, 500);
        param->setDefaultValue(ROTO_DEFAULT_FEATHER);
        _imp->knobsTable->addPerItemKnobMaster(param);
        shapePage->addKnob(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoFeatherFallOffParam);
        param->setLabel(tr(kRotoFeatherFallOffParamLabel));
        param->setHintToolTip( tr(kRotoFeatherFallOffHint) );
        param->setRange(0.001, 5.);
        param->setDisplayRange(0.2, 5.);
        param->setDefaultValue(ROTO_DEFAULT_FEATHERFALLOFF);
        _imp->knobsTable->addPerItemKnobMaster(param);
        param->setAddNewLine(false);
        shapePage->addKnob(param);
    }


    {
        KnobChoicePtr param = createKnob<KnobChoice>(kRotoFeatherFallOffType);
        param->setLabel(tr(kRotoFeatherFallOffTypeLabel));
        param->setHintToolTip( tr(kRotoFeatherFallOffTypeHint) );
        {
            std::vector<ChoiceOption> entries;
            entries.push_back(ChoiceOption(kRotoFeatherFallOffTypeLinear, "", tr(kRotoFeatherFallOffTypeLinearHint).toStdString()));
            entries.push_back(ChoiceOption(kRotoFeatherFallOffTypePLinear, "", tr(kRotoFeatherFallOffTypePLinearHint).toStdString()));
            entries.push_back(ChoiceOption(kRotoFeatherFallOffTypeEaseIn, "", tr(kRotoFeatherFallOffTypeEaseInHint).toStdString()));
            entries.push_back(ChoiceOption(kRotoFeatherFallOffTypeEaseOut, "", tr(kRotoFeatherFallOffTypeEaseOutHint).toStdString()));
            entries.push_back(ChoiceOption(kRotoFeatherFallOffTypeSmooth, "", tr(kRotoFeatherFallOffTypeLinearHint).toStdString()));
            entries.push_back(ChoiceOption(kRotoFeatherFallOffTypeLinear, "", tr(kRotoFeatherFallOffTypeSmoothHint).toStdString()));

            param->populateChoices(entries);
        }
        shapePage->addKnob(param);
        _imp->knobsTable->addPerItemKnobMaster(param);
    }


} // initShapePageKnobs

void
RotoPaint::initStrokePageKnobs()
{
    EffectInstancePtr effect = shared_from_this();

    KnobPagePtr strokePage = getOrCreateKnob<KnobPage>(kRotoPaintStrokePageParam);
    strokePage->setLabel(tr(kRotoPaintStrokePageParamLabel));


    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoBrushSizeParam);
        param->setLabel(tr(kRotoBrushSizeParamLabel));
        param->setHintToolTip( tr(kRotoBrushSizeParamHint) );
        param->setDefaultValue(25);
        param->setRange(1., 1000);
        _imp->knobsTable->addPerItemKnobMaster(param);
        strokePage->addKnob(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoBrushSpacingParam);
        param->setLabel(tr(kRotoBrushSpacingParamLabel));
        param->setHintToolTip( tr(kRotoBrushSpacingParamHint) );
        param->setDefaultValue(0.1);
        param->setRange(0., 1.);
        _imp->knobsTable->addPerItemKnobMaster(param);
        strokePage->addKnob(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoBrushHardnessParam);
        param->setLabel(tr(kRotoBrushHardnessParamLabel));
        param->setHintToolTip( tr(kRotoBrushHardnessParamHint) );
        param->setDefaultValue(0.2);
        param->setRange(0., 1.);
        _imp->knobsTable->addPerItemKnobMaster(param);
        strokePage->addKnob(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoBrushEffectParam);
        param->setLabel(tr(kRotoBrushEffectParamLabel));
        param->setHintToolTip( tr(kRotoBrushEffectParamHint) );
        param->setDefaultValue(15);
        param->setRange(0., 100.);
        _imp->knobsTable->addPerItemKnobMaster(param);
        strokePage->addKnob(param);
    }

    {
        KnobSeparatorPtr param = createKnob<KnobSeparator>(kRotoBrushPressureLabelParam);
        param->setLabel(tr(kRotoBrushPressureLabelParamLabel));
        param->setHintToolTip( tr(kRotoBrushPressureLabelParamHint) );
        _imp->knobsTable->addPerItemKnobMaster(param);
        strokePage->addKnob(param);
    }

    {
        KnobBoolPtr param = createKnob<KnobBool>(kRotoBrushPressureOpacityParam);
        param->setLabel(tr(kRotoBrushPressureOpacityParamLabel));
        param->setHintToolTip( tr(kRotoBrushPressureOpacityParamHint) );
        param->setAnimationEnabled(false);
        param->setDefaultValue(true);
        param->setAddNewLine(false);
        _imp->knobsTable->addPerItemKnobMaster(param);
        strokePage->addKnob(param);
    }

    {
        KnobBoolPtr param = createKnob<KnobBool>(kRotoBrushPressureSizeParam);
        param->setLabel(tr(kRotoBrushPressureSizeParamLabel));
        param->setHintToolTip( tr(kRotoBrushPressureSizeParamHint) );
        param->setAnimationEnabled(false);
        param->setDefaultValue(false);
        param->setAddNewLine(false);
        _imp->knobsTable->addPerItemKnobMaster(param);
        strokePage->addKnob(param);
    }

    {
        KnobBoolPtr param = createKnob<KnobBool>(kRotoBrushPressureHardnessParam);
        param->setLabel(tr(kRotoBrushPressureHardnessParamLabel));
        param->setHintToolTip( tr(kRotoBrushPressureHardnessParamHint) );
        param->setAnimationEnabled(false);
        param->setDefaultValue(false);
        param->setAddNewLine(true);
        _imp->knobsTable->addPerItemKnobMaster(param);
        strokePage->addKnob(param);
    }

    {
        KnobBoolPtr param = createKnob<KnobBool>(kRotoBrushBuildupParam);
        param->setLabel(tr(kRotoBrushBuildupParamLabel));
        param->setHintToolTip( tr(kRotoBrushBuildupParamHint) );
        param->setAnimationEnabled(false);
        param->setDefaultValue(false);
        param->setAddNewLine(true);
        _imp->knobsTable->addPerItemKnobMaster(param);
        strokePage->addKnob(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoBrushVisiblePortionParam, 2);
        param->setLabel(tr(kRotoBrushVisiblePortionParamLabel));
        param->setHintToolTip( tr(kRotoBrushVisiblePortionParamHint) );
        param->setDefaultValue(0, DimSpec(0));
        param->setDefaultValue(1, DimSpec(1));
        std::vector<double> mins, maxs;
        mins.push_back(0);
        mins.push_back(0);
        maxs.push_back(1);
        maxs.push_back(1);
        param->setRangeAcrossDimensions(mins, maxs);
        strokePage->addKnob(param);
        param->setDimensionName(DimIdx(0), tr("start").toStdString());
        param->setDimensionName(DimIdx(1), tr("end").toStdString());
        _imp->knobsTable->addPerItemKnobMaster(param);
    }
    
} // initStrokePageKnobs

void
RotoPaint::initTransformPageKnobs()
{
    EffectInstancePtr effect = shared_from_this();

    KnobPagePtr transformPage = getOrCreateKnob<KnobPage>(kRotoPaintTransformPageParam);
    transformPage->setLabel(tr(kRotoPaintTransformPageParamLabel));

    KnobDoublePtr translateKnob, scaleKnob, rotateKnob, skewXKnob, skewYKnob, centerKnob;
    KnobBoolPtr scaleUniformKnob;
    KnobChoicePtr skewOrderKnob;
    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoDrawableItemTranslateParam, 2);
        param->setLabel(tr(kRotoDrawableItemTranslateParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemTranslateParamHint) );
        param->setIncrement(10);
        translateKnob = param;
        transformPage->addKnob(param);
        _imp->translateKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoDrawableItemRotateParam);
        param->setLabel(tr(kRotoDrawableItemRotateParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemRotateParamHint) );
        param->setDisplayRange(-180, 180);
        rotateKnob = param;
        transformPage->addKnob(param);
        _imp->rotateKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoDrawableItemScaleParam, 2);
        param->setLabel(tr(kRotoDrawableItemScaleParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemScaleParamHint) );
        param->setDefaultValue(1, DimSpec(0));
        param->setDefaultValue(1, DimSpec(1));
        param->setDisplayRange(0.1, 10., DimSpec(0));
        param->setDisplayRange(0.1, 10., DimSpec(1));
        param->setAddNewLine(false);
        scaleKnob = param;
        transformPage->addKnob(param);
        _imp->scaleKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobBoolPtr param = createKnob<KnobBool>(kRotoDrawableItemScaleUniformParam);
        param->setLabel(tr(kRotoDrawableItemScaleUniformParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemScaleUniformParamHint) );
        param->setDefaultValue(true);
        param->setAnimationEnabled(false);
        scaleUniformKnob = param;
        transformPage->addKnob(param);
        _imp->scaleUniformKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoDrawableItemSkewXParam);
        param->setLabel(tr(kRotoDrawableItemSkewXParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemSkewXParamHint) );
        param->setDisplayRange(-1, 1);
        skewXKnob = param;
        transformPage->addKnob(param);
        _imp->skewXKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }
    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoDrawableItemSkewYParam);
        param->setLabel(tr(kRotoDrawableItemSkewYParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemSkewYParamHint) );
        param->setDisplayRange(-1, 1);
        skewYKnob = param;
        transformPage->addKnob(param);
        _imp->skewYKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobChoicePtr param = createKnob<KnobChoice>(kRotoDrawableItemSkewOrderParam);
        param->setLabel(tr(kRotoDrawableItemSkewOrderParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemSkewOrderParamHint) );
        param->setDefaultValue(0);
        {
            std::vector<ChoiceOption> choices;
            choices.push_back(ChoiceOption("XY", "", ""));
            choices.push_back(ChoiceOption("YX", "", ""));
            param->populateChoices(choices);
        }
        param->setAnimationEnabled(false);
        skewOrderKnob = param;
        transformPage->addKnob(param);
        _imp->skewOrderKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoDrawableItemCenterParam, 2);
        param->setLabel(tr(kRotoDrawableItemCenterParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemCenterParamHint) );
        param->setDefaultValuesAreNormalized(true);
        param->setAddNewLine(false);
        param->setDefaultValue(0.5, DimSpec(0));
        param->setDefaultValue(0.5, DimSpec(1));
        centerKnob = param;
        transformPage->addKnob(param);
        _imp->centerKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobButtonPtr param = createKnob<KnobButton>(kRotoResetCenterParam);
        param->setLabel(tr(kRotoResetCenterParamLabel));
        param->setHintToolTip( tr(kRotoResetCenterParamHint) );
        transformPage->addKnob(param);
        _imp->knobsTable->addPerItemKnobMaster(param);
        _imp->resetCenterKnob = param;
    }

    {
        KnobBoolPtr param = createKnob<KnobBool>(kRotoTransformInteractive);
        param->setLabel(tr(kRotoTransformInteractiveLabel));
        param->setHintToolTip(tr(kRotoTransformInteractiveHint));
        param->setDefaultValue(true);
        transformPage->addKnob(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoDrawableItemExtraMatrixParam, 9);
        param->setLabel(tr(kRotoDrawableItemExtraMatrixParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemExtraMatrixParamHint) );
        // Set to identity
        param->setDefaultValue(1, DimSpec(0));
        param->setDefaultValue(1, DimSpec(4));
        param->setDefaultValue(1, DimSpec(8));
        transformPage->addKnob(param);
        _imp->extraMatrixKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobButtonPtr param = createKnob<KnobButton>(kRotoResetTransformParam);
        param->setLabel(tr(kRotoResetTransformParamLabel));
        param->setHintToolTip( tr(kRotoResetTransformParamHint) );
        transformPage->addKnob(param);
        _imp->resetTransformKnob = param;
    }

    getNode()->addTransformInteract(translateKnob,
                                    scaleKnob,
                                    scaleUniformKnob,
                                    rotateKnob,
                                    skewXKnob,
                                    skewYKnob,
                                    skewOrderKnob,
                                    centerKnob,
                                    KnobBoolPtr() /*invert*/,
                                    KnobBoolPtr() /*interactive*/);

} // initTransformPageKnobs

void
RotoPaint::initCompNodeKnobs(const KnobPagePtr& page)
{
    EffectInstancePtr effect = shared_from_this();


    initLifeTimeKnobs(page);

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kLayeredCompMixParam);
        param->setLabel(tr(kLayeredCompMixParamLabel));
        param->setHintToolTip( tr(kLayeredCompMixParamHint) );
        param->setRange(0., 1.);
        param->setDefaultValue(1.);
        _imp->knobsTable->addPerItemKnobMaster(param);
        page->addKnob(param);
        _imp->mixKnob = param;
    }
    {
        KnobButtonPtr param = createKnob<KnobButton>(kRotoAddLayerParam);
        param->setHintToolTip( tr(kRotoAddLayerParamHint) );
        param->setLabel(tr(kRotoAddLayerParamLabel));
        param->setAddNewLine(false);
        _imp->addLayerButtonKnob = param;
        page->addKnob(param);
    }
    {
        KnobButtonPtr param = createKnob<KnobButton>(kRotoRemoveItemParam);
        param->setHintToolTip( tr(kRotoRemoveItemParamHint) );
        param->setLabel(tr(kRotoRemoveItemParamLabel));
        _imp->removeItemButtonKnob = param;
        page->addKnob(param);
    }

} // initCompNodeKnobs

void
RotoPaint::initClonePageKnobs()
{
    EffectInstancePtr effect = shared_from_this();

    KnobPagePtr clonePage = getOrCreateKnob<KnobPage>(kRotoPaintClonePageParam);
    clonePage->setLabel(tr(kRotoPaintClonePageParamLabel));
    {
        KnobChoicePtr param = createKnob<KnobChoice>(kRotoDrawableItemMergeAInputParam);
        param->setLabel(tr(kRotoDrawableItemMergeAInputParamLabel));
        param->setHintToolTip( tr(kRotoDrawableItemMergeAInputParamHint_RotoPaint) );
        param->setDefaultValue(1);
        clonePage->addKnob(param);
        _imp->mergeInputAChoiceKnob = param;
    }

    KnobDoublePtr cloneTranslateKnob, cloneRotateKnob, cloneScaleKnob, cloneSkewXKnob, cloneSkewYKnob, cloneCenterKnob;
    KnobBoolPtr cloneScaleUniformKnob;
    KnobChoicePtr cloneSkewOrderKnob;
    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoBrushTranslateParam, 2);
        param->setLabel(tr(kRotoBrushTranslateParamLabel));
        param->setHintToolTip( tr(kRotoBrushTranslateParamHint) );
        param->setIncrement(10);
        cloneTranslateKnob = param;
        clonePage->addKnob(param);
        _imp->cloneTranslateKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoBrushRotateParam);
        param->setLabel(tr(kRotoBrushRotateParamLabel));
        param->setHintToolTip( tr(kRotoBrushRotateParamHint) );
        param->setDisplayRange(-180, 180);
        cloneRotateKnob = param;
        clonePage->addKnob(param);
        _imp->cloneRotateKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoBrushScaleParam, 2);
        param->setLabel(tr(kRotoBrushScaleParamLabel));
        param->setHintToolTip( tr(kRotoBrushScaleParamHint) );
        param->setDefaultValue(1, DimSpec(0));
        param->setDefaultValue(1, DimSpec(1));
        param->setDisplayRange(0.1, 10., DimSpec(0));
        param->setDisplayRange(0.1, 10., DimSpec(1));
        param->setAddNewLine(false);
        cloneScaleKnob = param;
        clonePage->addKnob(param);
        _imp->cloneScaleKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobBoolPtr param = createKnob<KnobBool>(kRotoBrushScaleUniformParam);
        param->setLabel(tr(kRotoBrushScaleUniformParamLabel));
        param->setHintToolTip( tr(kRotoBrushScaleUniformParamHint) );
        param->setDefaultValue(true);
        param->setAnimationEnabled(false);
        cloneScaleUniformKnob = param;
        clonePage->addKnob(param);
        _imp->cloneUniformKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoBrushSkewXParam);
        param->setLabel(tr(kRotoBrushSkewXParamLabel));
        param->setHintToolTip( tr(kRotoBrushSkewXParamHint) );
        param->setDisplayRange(-1, 1, DimSpec(0));
        cloneSkewXKnob = param;
        clonePage->addKnob(param);
        _imp->cloneSkewXKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoBrushSkewYParam);
        param->setLabel(tr(kRotoBrushSkewYParamLabel));
        param->setHintToolTip( tr(kRotoBrushSkewYParamHint) );
        param->setDisplayRange(-1, 1);
        cloneSkewYKnob = param;
        clonePage->addKnob(param);
        _imp->cloneSkewYKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobChoicePtr param = createKnob<KnobChoice>(kRotoBrushSkewOrderParam);
        param->setLabel(tr(kRotoBrushSkewOrderParamLabel));
        param->setHintToolTip( tr(kRotoBrushSkewOrderParamHint) );
        param->setDefaultValue(0);
        {
            std::vector<ChoiceOption> choices;
            choices.push_back(ChoiceOption("XY", "", ""));
            choices.push_back(ChoiceOption("YX", "", ""));
            param->populateChoices(choices);
        }
        param->setAnimationEnabled(false);
        cloneSkewOrderKnob = param;
        clonePage->addKnob(param);
        _imp->cloneSkewOrderKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoBrushCenterParam, 2);
        param->setLabel(tr(kRotoBrushCenterParamLabel));
        param->setHintToolTip( tr(kRotoBrushCenterParamHint) );
        param->setDefaultValuesAreNormalized(true);
        param->setAddNewLine(false);
        param->setDefaultValue(0.5, DimSpec(0));
        param->setDefaultValue(0.5, DimSpec(1));
        cloneCenterKnob = param;
        clonePage->addKnob(param);
        _imp->cloneCenterKnob = param;
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobButtonPtr param = createKnob<KnobButton>(kRotoResetCloneCenterParam);
        param->setLabel(tr(kRotoResetCloneCenterParamLabel));
        param->setHintToolTip( tr(kRotoResetCloneCenterParamHint) );
        clonePage->addKnob(param);
        _imp->resetCloneCenterKnob = param;
    }

    {
        KnobButtonPtr param = createKnob<KnobButton>(kRotoResetCloneTransformParam);
        param->setLabel(tr(kRotoResetCloneTransformParamLabel));
        param->setHintToolTip( tr(kRotoResetCloneTransformParamHint) );
        clonePage->addKnob(param);
        _imp->resetCloneTransformKnob = param;

    }


    getNode()->addTransformInteract(cloneTranslateKnob,
                                    cloneScaleKnob,
                                    cloneScaleUniformKnob,
                                    cloneRotateKnob,
                                    cloneSkewXKnob,
                                    cloneSkewYKnob,
                                    cloneSkewOrderKnob,
                                    cloneCenterKnob,
                                    KnobBoolPtr() /*invert*/,
                                    KnobBoolPtr() /*interactive*/);

    {
        KnobChoicePtr param = createKnob<KnobChoice>(kRotoBrushFilterParam);
        param->setLabel(tr(kRotoBrushFilterParamLabel));
        param->setHintToolTip( tr(kRotoBrushFilterParamHint) );
        {
            std::vector<ChoiceOption> choices;
            choices.push_back(ChoiceOption(kFilterImpulse, "", tr(kFilterImpulseHint).toStdString()));
            choices.push_back(ChoiceOption(kFilterBilinear, "", tr(kFilterBilinearHint).toStdString()));
            choices.push_back(ChoiceOption(kFilterCubic, "", tr(kFilterCubicHint).toStdString()));
            choices.push_back(ChoiceOption(kFilterKeys, "", tr(kFilterKeysHint).toStdString()));
            choices.push_back(ChoiceOption(kFilterSimon, "", tr(kFilterSimonHint).toStdString()));
            choices.push_back(ChoiceOption(kFilterRifman, "", tr(kFilterRifmanHint).toStdString()));
            choices.push_back(ChoiceOption(kFilterMitchell, "", tr(kFilterMitchellHint).toStdString()));
            choices.push_back(ChoiceOption(kFilterParzen, "", tr(kFilterParzenHint).toStdString()));
            choices.push_back(ChoiceOption(kFilterNotch, "", tr(kFilterNotchHint).toStdString()));

            param->populateChoices(choices);
        }
        param->setDefaultValue(2);
        param->setAddNewLine(false);
        clonePage->addKnob(param);
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobBoolPtr param = createKnob<KnobBool>(kRotoBrushBlackOutsideParam);
        param->setLabel(tr(kRotoBrushBlackOutsideParamLabel));
        param->setHintToolTip( tr(kRotoBrushBlackOutsideParamHint) );
        param->setDefaultValue(true);
        clonePage->addKnob(param);
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobIntPtr param = createKnob<KnobInt>(kRotoBrushTimeOffsetParam);
        param->setLabel(tr(kRotoBrushTimeOffsetParamLabel));
        param->setHintToolTip( tr(kRotoBrushTimeOffsetParamHint_Clone) );
        param->setDisplayRange(-100, 100);
        param->setAddNewLine(false);
        clonePage->addKnob(param);
        _imp->knobsTable->addPerItemKnobMaster(param);
    }

    {
        KnobChoicePtr param = createKnob<KnobChoice>(kRotoBrushTimeOffsetModeParam);
        param->setLabel(tr(kRotoBrushTimeOffsetModeParamLabel));
        param->setHintToolTip( tr(kRotoBrushTimeOffsetModeParamHint) );
        {
            std::vector<ChoiceOption> modes;
            modes.push_back(ChoiceOption("Relative", "", ""));
            modes.push_back(ChoiceOption("Absolute", "", ""));
            param->populateChoices(modes);
        }
        clonePage->addKnob(param);
        _imp->knobsTable->addPerItemKnobMaster(param);
    }
    
} // initClonePageKnobs

void
RotoPaint::initMotionBlurPageKnobs()
{
    EffectInstancePtr effect = shared_from_this();

    KnobPagePtr mbPage = getOrCreateKnob<KnobPage>(kRotoPaintMotionBlurPageParam);
    mbPage->setLabel(tr(kRotoPaintMotionBlurPageParamLabel));

    {
        KnobChoicePtr param = createKnob<KnobChoice>(kRotoMotionBlurModeParam);
        param->setLabel(tr(kRotoMotionBlurModeParamLabel));
        param->setHintToolTip( tr(kRotoMotionBlurModeParamHint) );
        param->setAnimationEnabled(false);
        {
            std::vector<ChoiceOption> entries;
            assert((int)entries.size() == eRotoMotionBlurModeNone);
            entries.push_back(ChoiceOption(kRotoMotionBlurModeNone, "", tr(kRotoMotionBlurModeNoneHint).toStdString()));
            assert((int)entries.size() == eRotoMotionBlurModePerShape);
            entries.push_back(ChoiceOption(kRotoMotionBlurModePerShape, "", tr(kRotoMotionBlurModePerShapeHint).toStdString()));
            assert((int)entries.size() == eRotoMotionBlurModeGlobal);
            entries.push_back(ChoiceOption(kRotoMotionBlurModeGlobal, "", tr(kRotoMotionBlurModeGlobalHint).toStdString()));
            param->populateChoices(entries);
        }
        mbPage->addKnob(param);
        _imp->motionBlurTypeKnob =  param;
    }

    {
        KnobIntPtr param = createKnob<KnobInt>(kRotoPerShapeMotionBlurParam);
        param->setLabel(tr(kRotoMotionBlurParamLabel));
        param->setHintToolTip( tr(kRotoMotionBlurParamHint) );
        param->setDefaultValue(1);
        param->setRange(1, INT_MAX);
        param->setDisplayRange(1, 10);
        mbPage->addKnob(param);
        _imp->knobsTable->addPerItemKnobMaster(param);
        _imp->motionBlurKnob = param;
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoPerShapeShutterParam);
        param->setLabel(tr(kRotoShutterParamLabel));
        param->setHintToolTip( tr(kRotoShutterParamHint) );
        param->setDefaultValue(0.5);
        param->setRange(0, 2);
        param->setDisplayRange(0, 2);
        mbPage->addKnob(param);
        _imp->knobsTable->addPerItemKnobMaster(param);
        _imp->shutterKnob = param;
    }

    {
        KnobChoicePtr param = createKnob<KnobChoice>(kRotoPerShapeShutterOffsetTypeParam);
        param->setLabel(tr(kRotoShutterOffsetTypeParamLabel));
        param->setHintToolTip( tr(kRotoShutterOffsetTypeParamHint) );
        param->setDefaultValue(0);
        {
            std::vector<ChoiceOption> options;
            options.push_back(ChoiceOption("Centered", "", tr(kRotoShutterOffsetCenteredHint).toStdString()));
            options.push_back(ChoiceOption("Start", "", tr(kRotoShutterOffsetStartHint).toStdString()));
            options.push_back(ChoiceOption("End", "", tr(kRotoShutterOffsetEndHint).toStdString()));
            options.push_back(ChoiceOption("Custom", "", tr(kRotoShutterOffsetCustomHint).toStdString()));

            param->populateChoices(options);
        }
        param->setAddNewLine(false);
        mbPage->addKnob(param);
        _imp->knobsTable->addPerItemKnobMaster(param);
        _imp->shutterTypeKnob = param;
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoPerShapeShutterCustomOffsetParam);
        param->setLabel(tr(kRotoShutterCustomOffsetParamLabel));
        param->setHintToolTip( tr(kRotoShutterCustomOffsetParamHint) );
        param->setDefaultValue(0);
        mbPage->addKnob(param);
        _imp->knobsTable->addPerItemKnobMaster(param);
        _imp->customOffsetKnob = param;
    }

    {
        KnobIntPtr param = createKnob<KnobInt>(kRotoGlobalMotionBlurParam);
        param->setLabel(tr(kRotoMotionBlurParamLabel));
        param->setHintToolTip( tr(kRotoMotionBlurParamHint) );
        param->setDefaultValue(1);
        param->setRange(1, INT_MAX);
        param->setDisplayRange(1, 10);
        param->setSecret(true);
        mbPage->addKnob(param);
        _imp->globalMotionBlurKnob = param;
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoGlobalShutterParam);
        param->setLabel(tr(kRotoShutterParamLabel));
        param->setHintToolTip( tr(kRotoShutterParamHint) );
        param->setDefaultValue(0.5);
        param->setRange(0, 2);
        param->setDisplayRange(0, 2);
        param->setSecret(true);
        mbPage->addKnob(param);
        _imp->globalShutterKnob = param;
    }

    {
        KnobChoicePtr param = createKnob<KnobChoice>(kRotoGlobalShutterOffsetTypeParam);
        param->setLabel(tr(kRotoShutterOffsetTypeParamLabel));
        param->setHintToolTip( tr(kRotoShutterOffsetTypeParamHint) );
        param->setDefaultValue(0);
        {
            std::vector<ChoiceOption> options;
            options.push_back(ChoiceOption("Centered", "", tr(kRotoShutterOffsetCenteredHint).toStdString()));
            options.push_back(ChoiceOption("Start", "", tr(kRotoShutterOffsetStartHint).toStdString()));
            options.push_back(ChoiceOption("End", "", tr(kRotoShutterOffsetEndHint).toStdString()));
            options.push_back(ChoiceOption("Custom", "", tr(kRotoShutterOffsetCustomHint).toStdString()));

            param->populateChoices(options);
        }
        param->setAddNewLine(false);
        param->setSecret(true);
        mbPage->addKnob(param);
        _imp->globalShutterTypeKnob = param;
    }

    {
        KnobDoublePtr param = createKnob<KnobDouble>(kRotoGlobalShutterCustomOffsetParam);
        param->setLabel(tr(kRotoShutterCustomOffsetParamLabel));
        param->setHintToolTip( tr(kRotoShutterCustomOffsetParamHint) );
        param->setDefaultValue(0);
        param->setSecret(true);
        mbPage->addKnob(param);
        _imp->globalCustomOffsetKnob = param;
    }

    _imp->refreshMotionBlurKnobsVisibility();
} // void initMotionBlurPageKnobs();

void
RotoPaint::setupInitialSubGraphState()
{
    RotoPaintPtr thisShared = boost::dynamic_pointer_cast<RotoPaint>(shared_from_this());
    if (_imp->nodeType != eRotoPaintTypeComp) {
        for (int i = 0; i < ROTOPAINT_MAX_INPUTS_COUNT; ++i) {

            std::stringstream ss;
            if (i == 0) {
                ss << "Bg";
            } else if (i == ROTOPAINT_MASK_INPUT_INDEX) {
                ss << "Mask";
            } else {
                ss << "Bg" << i + 1;
            }
            {
                CreateNodeArgsPtr args(CreateNodeArgs::create(PLUGINID_NATRON_INPUT, thisShared));
                args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
                args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
                args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, ss.str());
                args->addParamDefaultValue<bool>(kNatronGroupInputIsOptionalParamName, true);
                if (i == ROTOPAINT_MASK_INPUT_INDEX) {
                    args->addParamDefaultValue<bool>(kNatronGroupInputIsMaskParamName, true);

                }
                NodePtr input = getApp()->createNode(args);
                assert(input);
                _imp->inputNodes.push_back(input);
            }
        }
    } else {
        for (int i = 0; i < LAYERED_COMP_MAX_INPUTS_COUNT; ++i) {
            std::string inputName;
            bool isMask = false;
            {
                std::stringstream ss;
                if (i == 0) {
                    ss << "Bg";
                } else if (i < LAYERED_COMP_FIRST_MASK_INPUT_INDEX) {
                    ss << "Source";
                    if (i > 1) {
                        ss << i;
                    }
                } else {
                    isMask = true;
                    ss << "Mask";
                    if (i > LAYERED_COMP_FIRST_MASK_INPUT_INDEX) {
                        int nb = i - LAYERED_COMP_FIRST_MASK_INPUT_INDEX + 1;
                        ss << nb;
                    }
                }
                inputName = ss.str();
            }
            {
                CreateNodeArgsPtr args(CreateNodeArgs::create(PLUGINID_NATRON_INPUT, thisShared));
                args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
                args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
                args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, inputName);
                args->addParamDefaultValue<bool>(kNatronGroupInputIsOptionalParamName, true);
                if (isMask) {
                    args->addParamDefaultValue<bool>(kNatronGroupInputIsMaskParamName, true);

                }
                NodePtr input = getApp()->createNode(args);
                assert(input);
                _imp->inputNodes.push_back(input);
            }
        }
    }
    NodePtr outputNode;
    {
        CreateNodeArgsPtr args(CreateNodeArgs::create(PLUGINID_NATRON_OUTPUT, thisShared));
        args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
        args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
        args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, "Output");

        outputNode = getApp()->createNode(args);
        assert(outputNode);
    }
    NodePtr premultNode;
    NodePtr noopNode;
    if (_imp->nodeType == eRotoPaintTypeRoto || _imp->nodeType == eRotoPaintTypeRotoPaint) {
        {
            CreateNodeArgsPtr args(CreateNodeArgs::create(PLUGINID_OFX_PREMULT, thisShared));
            args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
            args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
            // Set premult node to be identity by default
            args->addParamDefaultValue<bool>(kNatronOfxParamProcessR, false);
            args->addParamDefaultValue<bool>(kNatronOfxParamProcessG, false);
            args->addParamDefaultValue<bool>(kNatronOfxParamProcessB, false);
            args->addParamDefaultValue<bool>(kNatronOfxParamProcessA, false);
            args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, "AlphaPremult");

            premultNode = getApp()->createNode(args);
            _imp->premultNode = premultNode;
            assert(premultNode);
            if (!premultNode) {
                throw std::runtime_error( tr("Rotopaint requires the plug-in %1 in order to work").arg( QLatin1String(PLUGINID_OFX_PREMULT) ).toStdString() );
            }

            if (_imp->premultKnob.lock()) {
                KnobBoolPtr disablePremultKnob = premultNode->getEffectInstance()->getDisabledKnob();
                try {
                    disablePremultKnob->setExpression(DimSpec::all(), ViewSetSpec::all(), "not thisGroup.premultiply.get()", false, true);
                } catch (...) {
                    assert(false);
                }
            }

        }

        // Make a no-op that fixes the output premultiplication state
        {
            CreateNodeArgsPtr args(CreateNodeArgs::create(PLUGINID_OFX_NOOP, thisShared));
            args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
            args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
            // Set premult node to be identity by default
            args->addParamDefaultValue<bool>("setPremult", true);
            noopNode = getApp()->createNode(args);
            _imp->premultFixerNode = noopNode;

            KnobIPtr premultChoiceKnob = noopNode->getKnobByName("outputPremult");
            try {
                const char* premultChoiceExpr =
                "premultChecked = thisGroup.premultiply.get()\n"
                "rChecked = thisGroup.doRed.get()\n"
                "gChecked = thisGroup.doGreen.get()\n"
                "bChecked = thisGroup.doBlue.get()\n"
                "aChecked = thisGroup.doAlpha.get()\n"
                "hasColor = rChecked or gChecked or bChecked\n"
                "ret = 0\n"
                "if premultChecked or hasColor or not aChecked:\n"
                "    ret = 1\n" // premult if there's one of RGB checked or none
                "else:\n"
                "    ret = 2\n"
                ;
                premultChoiceKnob->setExpression(DimSpec::all(), ViewSetSpec::all(), premultChoiceExpr, true, true);
            } catch (const std::exception& e) {
                std::cerr << e.what() << std::endl;
                assert(false);
            }
            
        }
    }
    if (noopNode && premultNode) {
        noopNode->connectInput(premultNode, 0);
        premultNode->connectInput(noopNode, 0);
    }

    // Initialize default connections
    outputNode->connectInput(_imp->inputNodes[0].lock(), 0);
} // setupInitialSubGraphState

void
RotoPaint::initViewerUIKnobs(const KnobPagePtr& generalPage)
{
    RotoPaintPtr thisShared = boost::dynamic_pointer_cast<RotoPaint>(shared_from_this());
    KnobButtonPtr autoKeyingEnabled = createKnob<KnobButton>(kRotoUIParamAutoKeyingEnabled);
    autoKeyingEnabled->setLabel(tr(kRotoUIParamAutoKeyingEnabledLabel));
    autoKeyingEnabled->setHintToolTip( tr(kRotoUIParamAutoKeyingEnabledHint) );
    autoKeyingEnabled->setEvaluateOnChange(false);
    autoKeyingEnabled->setCheckable(true);
    autoKeyingEnabled->setDefaultValue(true);
    autoKeyingEnabled->setSecret(true);
    autoKeyingEnabled->setInViewerContextCanHaveShortcut(true);
    autoKeyingEnabled->setIconLabel("Images/autoKeyingEnabled.png", true);
    autoKeyingEnabled->setIconLabel("Images/autoKeyingDisabled.png", false);
    generalPage->addKnob(autoKeyingEnabled);
    _imp->ui->autoKeyingEnabledButton = autoKeyingEnabled;

    KnobButtonPtr featherLinkEnabled = createKnob<KnobButton>(kRotoUIParamFeatherLinkEnabled);
    featherLinkEnabled->setLabel(tr(kRotoUIParamFeatherLinkEnabledLabel));
    featherLinkEnabled->setCheckable(true);
    featherLinkEnabled->setEvaluateOnChange(false);
    featherLinkEnabled->setDefaultValue(true);
    featherLinkEnabled->setSecret(true);
    featherLinkEnabled->setInViewerContextCanHaveShortcut(true);
    featherLinkEnabled->setHintToolTip( tr(kRotoUIParamFeatherLinkEnabledHint) );
    featherLinkEnabled->setIconLabel("Images/featherLinkEnabled.png", true);
    featherLinkEnabled->setIconLabel("Images/featherLinkDisabled.png", false);
    generalPage->addKnob(featherLinkEnabled);
    _imp->ui->featherLinkEnabledButton = featherLinkEnabled;

    KnobButtonPtr displayFeatherEnabled = createKnob<KnobButton>(kRotoUIParamDisplayFeather);
    displayFeatherEnabled->setLabel(tr(kRotoUIParamDisplayFeatherLabel));
    displayFeatherEnabled->setHintToolTip( tr(kRotoUIParamDisplayFeatherHint) );
    displayFeatherEnabled->setEvaluateOnChange(false);
    displayFeatherEnabled->setCheckable(true);
    displayFeatherEnabled->setDefaultValue(true);
    displayFeatherEnabled->setSecret(true);
    displayFeatherEnabled->setInViewerContextCanHaveShortcut(true);
    addOverlaySlaveParam(displayFeatherEnabled);
    displayFeatherEnabled->setIconLabel("Images/featherEnabled.png", true);
    displayFeatherEnabled->setIconLabel("Images/featherDisabled.png", false);
    generalPage->addKnob(displayFeatherEnabled);
    _imp->ui->displayFeatherEnabledButton = displayFeatherEnabled;

    KnobButtonPtr stickySelection = createKnob<KnobButton>(kRotoUIParamStickySelectionEnabled);
    stickySelection->setLabel(tr(kRotoUIParamStickySelectionEnabledLabel));
    stickySelection->setHintToolTip( tr(kRotoUIParamStickySelectionEnabledHint) );
    stickySelection->setEvaluateOnChange(false);
    stickySelection->setCheckable(true);
    stickySelection->setDefaultValue(false);
    stickySelection->setSecret(true);
    stickySelection->setInViewerContextCanHaveShortcut(true);
    stickySelection->setIconLabel("Images/stickySelectionEnabled.png", true);
    stickySelection->setIconLabel("Images/stickySelectionDisabled.png", false);
    generalPage->addKnob(stickySelection);
    _imp->ui->stickySelectionEnabledButton = stickySelection;

    KnobButtonPtr bboxClickAnywhere = createKnob<KnobButton>(kRotoUIParamStickyBbox);
    bboxClickAnywhere->setLabel(tr(kRotoUIParamStickyBboxLabel));
    bboxClickAnywhere->setHintToolTip( tr(kRotoUIParamStickyBboxHint) );
    bboxClickAnywhere->setEvaluateOnChange(false);
    bboxClickAnywhere->setCheckable(true);
    bboxClickAnywhere->setDefaultValue(false);
    bboxClickAnywhere->setSecret(true);
    bboxClickAnywhere->setInViewerContextCanHaveShortcut(true);
    bboxClickAnywhere->setIconLabel("Images/viewer_roiEnabled.png", true);
    bboxClickAnywhere->setIconLabel("Images/viewer_roiDisabled.png", false);
    generalPage->addKnob(bboxClickAnywhere);
    _imp->ui->bboxClickAnywhereButton = bboxClickAnywhere;

    KnobButtonPtr rippleEditEnabled = createKnob<KnobButton>(kRotoUIParamRippleEdit);
    rippleEditEnabled->setLabel(tr(kRotoUIParamRippleEditLabel));
    rippleEditEnabled->setHintToolTip( tr(kRotoUIParamRippleEditHint) );
    rippleEditEnabled->setEvaluateOnChange(false);
    rippleEditEnabled->setCheckable(true);
    rippleEditEnabled->setDefaultValue(false);
    rippleEditEnabled->setSecret(true);
    rippleEditEnabled->setInViewerContextCanHaveShortcut(true);
    rippleEditEnabled->setIconLabel("Images/rippleEditEnabled.png", true);
    rippleEditEnabled->setIconLabel("Images/rippleEditDisabled.png", false);
    generalPage->addKnob(rippleEditEnabled);
    _imp->ui->rippleEditEnabledButton = rippleEditEnabled;

    KnobButtonPtr addKeyframe = createKnob<KnobButton>(kRotoUIParamAddKeyFrame);
    addKeyframe->setLabel(tr(kRotoUIParamAddKeyFrameLabel));
    addKeyframe->setEvaluateOnChange(false);
    addKeyframe->setHintToolTip( tr(kRotoUIParamAddKeyFrameHint) );
    addKeyframe->setSecret(true);
    addKeyframe->setInViewerContextCanHaveShortcut(true);
    addKeyframe->setIconLabel("Images/addKF.png");
    generalPage->addKnob(addKeyframe);
    _imp->ui->addKeyframeButton = addKeyframe;

    KnobButtonPtr removeKeyframe = createKnob<KnobButton>(kRotoUIParamRemoveKeyframe);
    removeKeyframe->setLabel(tr(kRotoUIParamRemoveKeyframeLabel));
    removeKeyframe->setHintToolTip( tr(kRotoUIParamRemoveKeyframeHint) );
    removeKeyframe->setEvaluateOnChange(false);
    removeKeyframe->setSecret(true);
    removeKeyframe->setInViewerContextCanHaveShortcut(true);
    removeKeyframe->setIconLabel("Images/removeKF.png");
    generalPage->addKnob(removeKeyframe);
    _imp->ui->removeKeyframeButton = removeKeyframe;

    KnobButtonPtr showTransform = createKnob<KnobButton>(kRotoUIParamShowTransform);
    showTransform->setLabel(tr(kRotoUIParamShowTransformLabel));
    showTransform->setHintToolTip( tr(kRotoUIParamShowTransformHint) );
    showTransform->setEvaluateOnChange(false);
    showTransform->setSecret(true);
    showTransform->setCheckable(true);
    showTransform->setDefaultValue(true);
    showTransform->setInViewerContextCanHaveShortcut(true);
    showTransform->setInViewerContextLayoutType(eViewerContextLayoutTypeAddNewLine);
    showTransform->setIconLabel("Images/rotoShowTransformOverlay.png", true);
    showTransform->setIconLabel("Images/rotoHideTransformOverlay.png", false);
    generalPage->addKnob(showTransform);
    addOverlaySlaveParam(showTransform);
    _imp->ui->showTransformHandle = showTransform;

    // RotoPaint

    KnobBoolPtr multiStroke = createKnob<KnobBool>(kRotoUIParamMultiStrokeEnabled);
    multiStroke->setLabel(tr(kRotoUIParamMultiStrokeEnabledLabel));
    multiStroke->setInViewerContextLabel( tr(kRotoUIParamMultiStrokeEnabledLabel) );
    multiStroke->setHintToolTip( tr(kRotoUIParamMultiStrokeEnabledHint) );
    multiStroke->setEvaluateOnChange(false);
    multiStroke->setDefaultValue(true);
    multiStroke->setSecret(true);
    generalPage->addKnob(multiStroke);
    _imp->ui->multiStrokeEnabled = multiStroke;

    KnobColorPtr colorWheel = createKnob<KnobColor>(kRotoUIParamColorWheel, 4);
    colorWheel->setLabel(tr(kRotoUIParamColorWheelLabel));
    colorWheel->setHintToolTip( tr(kRotoUIParamColorWheelHint) );
    colorWheel->setEvaluateOnChange(false);
    colorWheel->setDefaultValue(1., DimIdx(0));
    colorWheel->setDefaultValue(1., DimIdx(1));
    colorWheel->setDefaultValue(1., DimIdx(2));
    colorWheel->setDefaultValue(1., DimIdx(3));
    colorWheel->setSecret(true);
    generalPage->addKnob(colorWheel);
    _imp->ui->colorWheelButton = colorWheel;

    KnobChoicePtr blendingModes = createKnob<KnobChoice>(kRotoUIParamBlendingOp);
    blendingModes->setLabel(tr(kRotoUIParamBlendingOpLabel));
    blendingModes->setHintToolTip( tr(kRotoUIParamBlendingOpHint) );
    blendingModes->setEvaluateOnChange(false);
    blendingModes->setSecret(true);
    {
        std::vector<ChoiceOption> choices;
        Merge::getOperatorStrings(&choices);
        blendingModes->populateChoices(choices);
    }
    blendingModes->setDefaultValue( (int)eMergeOver );
    generalPage->addKnob(blendingModes);
    _imp->ui->compositingOperatorChoice = blendingModes;


    KnobDoublePtr opacityKnob = createKnob<KnobDouble>(kRotoUIParamOpacity);
    opacityKnob->setLabel(tr(kRotoUIParamOpacityLabel));
    opacityKnob->setInViewerContextLabel( tr(kRotoUIParamOpacityLabel) );
    opacityKnob->setHintToolTip( tr(kRotoUIParamOpacityHint) );
    opacityKnob->setEvaluateOnChange(false);
    opacityKnob->setSecret(true);
    opacityKnob->setDefaultValue(1.);
    opacityKnob->setRange(0., 1.);
    opacityKnob->disableSlider();
    generalPage->addKnob(opacityKnob);
    _imp->ui->opacitySpinbox = opacityKnob;

    KnobButtonPtr pressureOpacity = createKnob<KnobButton>(kRotoUIParamPressureOpacity);
    pressureOpacity->setLabel(tr(kRotoUIParamPressureOpacityLabel));
    pressureOpacity->setHintToolTip( tr(kRotoUIParamPressureOpacityHint) );
    pressureOpacity->setEvaluateOnChange(false);
    pressureOpacity->setCheckable(true);
    pressureOpacity->setDefaultValue(true);
    pressureOpacity->setSecret(true);
    pressureOpacity->setInViewerContextCanHaveShortcut(true);
    pressureOpacity->setIconLabel("Images/rotopaint_pressure_on.png", true);
    pressureOpacity->setIconLabel("Images/rotopaint_pressure_off.png", false);
    generalPage->addKnob(pressureOpacity);
    _imp->ui->pressureOpacityButton = pressureOpacity;

    KnobDoublePtr sizeKnob = createKnob<KnobDouble>(kRotoUIParamSize);
    sizeKnob->setLabel(tr(kRotoUIParamSizeLabel));
    sizeKnob->setInViewerContextLabel( tr(kRotoUIParamSizeLabel) );
    sizeKnob->setHintToolTip( tr(kRotoUIParamSizeHint) );
    sizeKnob->setEvaluateOnChange(false);
    sizeKnob->setSecret(true);
    sizeKnob->setDefaultValue(25.);
    sizeKnob->setRange(0., 1000.);
    sizeKnob->disableSlider();
    generalPage->addKnob(sizeKnob);
    _imp->ui->sizeSpinbox = sizeKnob;

    KnobButtonPtr pressureSize = createKnob<KnobButton>(kRotoUIParamPressureSize);
    pressureSize->setLabel(tr(kRotoUIParamPressureSizeLabel));
    pressureSize->setHintToolTip( tr(kRotoUIParamPressureSizeHint) );
    pressureSize->setEvaluateOnChange(false);
    pressureSize->setCheckable(true);
    pressureSize->setDefaultValue(false);
    pressureSize->setSecret(true);
    pressureSize->setInViewerContextCanHaveShortcut(true);
    pressureSize->setIconLabel("Images/rotopaint_pressure_on.png", true);
    pressureSize->setIconLabel("Images/rotopaint_pressure_off.png", false);
    generalPage->addKnob(pressureSize);
    _imp->ui->pressureSizeButton = pressureSize;

    KnobDoublePtr hardnessKnob = createKnob<KnobDouble>(kRotoUIParamHardness);
    hardnessKnob->setLabel(tr(kRotoUIParamHardnessLabel));
    hardnessKnob->setInViewerContextLabel( tr(kRotoUIParamHardnessLabel) );
    hardnessKnob->setHintToolTip( tr(kRotoUIParamHardnessHint) );
    hardnessKnob->setEvaluateOnChange(false);
    hardnessKnob->setSecret(true);
    hardnessKnob->setDefaultValue(.2);
    hardnessKnob->setRange(0., 1.);
    hardnessKnob->disableSlider();
    generalPage->addKnob(hardnessKnob);
    _imp->ui->hardnessSpinbox = hardnessKnob;

    KnobButtonPtr pressureHardness = createKnob<KnobButton>(kRotoUIParamPressureHardness);
    pressureHardness->setLabel(tr(kRotoUIParamPressureHardnessLabel));
    pressureHardness->setHintToolTip( tr(kRotoUIParamPressureHardnessHint) );
    pressureHardness->setEvaluateOnChange(false);
    pressureHardness->setCheckable(true);
    pressureHardness->setDefaultValue(false);
    pressureHardness->setSecret(true);
    pressureHardness->setInViewerContextCanHaveShortcut(true);
    pressureHardness->setIconLabel("Images/rotopaint_pressure_on.png", true);
    pressureHardness->setIconLabel("Images/rotopaint_pressure_off.png", false);
    generalPage->addKnob(pressureHardness);
    _imp->ui->pressureHardnessButton = pressureHardness;

    KnobButtonPtr buildUp = createKnob<KnobButton>(kRotoUIParamBuildUp);
    buildUp->setLabel(tr(kRotoUIParamBuildUpLabel));
    buildUp->setInViewerContextLabel( tr(kRotoUIParamBuildUpLabel) );
    buildUp->setHintToolTip( tr(kRotoUIParamBuildUpHint) );
    buildUp->setEvaluateOnChange(false);
    buildUp->setCheckable(true);
    buildUp->setDefaultValue(true);
    buildUp->setSecret(true);
    buildUp->setInViewerContextCanHaveShortcut(true);
    buildUp->setIconLabel("Images/rotopaint_buildup_on.png", true);
    buildUp->setIconLabel("Images/rotopaint_buildup_off.png", false);
    generalPage->addKnob(buildUp);
    _imp->ui->buildUpButton = buildUp;

    KnobDoublePtr effectStrength = createKnob<KnobDouble>(kRotoUIParamEffect);
    effectStrength->setLabel(tr(kRotoUIParamEffectLabel));
    effectStrength->setInViewerContextLabel( tr(kRotoUIParamEffectLabel) );
    effectStrength->setHintToolTip( tr(kRotoUIParamEffectHint) );
    effectStrength->setEvaluateOnChange(false);
    effectStrength->setSecret(true);
    effectStrength->setDefaultValue(15);
    effectStrength->setRange(0., 100.);
    effectStrength->disableSlider();
    generalPage->addKnob(effectStrength);
    _imp->ui->effectSpinBox = effectStrength;

    KnobIntPtr timeOffsetSb = createKnob<KnobInt>(kRotoUIParamTimeOffset);
    timeOffsetSb->setLabel(tr(kRotoUIParamTimeOffsetLabel));
    timeOffsetSb->setInViewerContextLabel( tr(kRotoUIParamTimeOffsetLabel) );
    timeOffsetSb->setHintToolTip( tr(kRotoUIParamTimeOffsetHint) );
    timeOffsetSb->setEvaluateOnChange(false);
    timeOffsetSb->setSecret(true);
    timeOffsetSb->setDefaultValue(0);
    timeOffsetSb->disableSlider();
    generalPage->addKnob(timeOffsetSb);
    _imp->ui->timeOffsetSpinBox = timeOffsetSb;

    KnobChoicePtr timeOffsetMode = createKnob<KnobChoice>(kRotoUIParamTimeOffset);
    timeOffsetMode->setLabel(tr(kRotoUIParamTimeOffsetLabel));
    timeOffsetMode->setHintToolTip( tr(kRotoUIParamTimeOffsetHint) );
    timeOffsetMode->setEvaluateOnChange(false);
    timeOffsetMode->setSecret(true);
    {
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("Relative", "", tr("The time offset is a frame number in the source").toStdString()));
        choices.push_back(ChoiceOption("Absolute", "", tr("The time offset is a relative amount of frames relative to the current frame").toStdString()));
        timeOffsetMode->populateChoices(choices);
    }
    timeOffsetMode->setDefaultValue(0);
    generalPage->addKnob(timeOffsetMode);
    _imp->ui->timeOffsetModeChoice = timeOffsetMode;

    KnobChoicePtr sourceType = createKnob<KnobChoice>(kRotoUIParamSourceType);
    sourceType->setLabel(tr(kRotoUIParamSourceTypeLabel));
    sourceType->setHintToolTip( tr(kRotoUIParamSourceTypeHint) );
    sourceType->setEvaluateOnChange(false);
    sourceType->setSecret(true);
    {
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("foreground", "", ""));
        choices.push_back(ChoiceOption("background", "", ""));
        for (int i = 1; i < 10; ++i) {
            QString str = QString::fromUtf8("background") + QString::number(i + 1);
            choices.push_back( ChoiceOption(str.toStdString(), "", ""));
        }
        sourceType->populateChoices(choices);
    }
    sourceType->setDefaultValue(1);
    generalPage->addKnob(sourceType);
    _imp->ui->sourceTypeChoice = sourceType;


    KnobButtonPtr resetCloneOffset = createKnob<KnobButton>(kRotoUIParamResetCloneOffset);
    resetCloneOffset->setLabel(tr(kRotoUIParamResetCloneOffsetLabel));
    resetCloneOffset->setHintToolTip( tr(kRotoUIParamResetCloneOffsetHint) );
    resetCloneOffset->setEvaluateOnChange(false);
    resetCloneOffset->setSecret(true);
    resetCloneOffset->setInViewerContextCanHaveShortcut(true);
    resetCloneOffset->setInViewerContextLayoutType(eViewerContextLayoutTypeAddNewLine);
    generalPage->addKnob(resetCloneOffset);
    addOverlaySlaveParam(resetCloneOffset);
    _imp->ui->resetCloneOffsetButton = resetCloneOffset;


    // Roto
    addKnobToViewerUI(autoKeyingEnabled);
    addKnobToViewerUI(featherLinkEnabled);
    addKnobToViewerUI(displayFeatherEnabled);
    addKnobToViewerUI(stickySelection);
    addKnobToViewerUI(bboxClickAnywhere);
    addKnobToViewerUI(rippleEditEnabled);
    addKnobToViewerUI(addKeyframe);
    addKnobToViewerUI(removeKeyframe);
    addKnobToViewerUI(showTransform);

    // RotoPaint
    addKnobToViewerUI(multiStroke);
    multiStroke->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(colorWheel);
    colorWheel->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(blendingModes);
    blendingModes->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(opacityKnob);
    opacityKnob->setInViewerContextItemSpacing(1);
    addKnobToViewerUI(pressureOpacity);
    pressureOpacity->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(sizeKnob);
    sizeKnob->setInViewerContextItemSpacing(1);
    addKnobToViewerUI(pressureSize);
    pressureSize->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(hardnessKnob);
    hardnessKnob->setInViewerContextItemSpacing(1);
    addKnobToViewerUI(pressureHardness);
    pressureHardness->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(buildUp);
    buildUp->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(effectStrength);
    effectStrength->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(timeOffsetSb);
    timeOffsetSb->setInViewerContextItemSpacing(1);
    addKnobToViewerUI(timeOffsetMode);
    addKnobToViewerUI(sourceType);
    addKnobToViewerUI(resetCloneOffset);
    resetCloneOffset->setInViewerContextLayoutType(eViewerContextLayoutTypeStretchAfter);

    KnobPagePtr toolbar = createKnob<KnobPage>(std::string(kRotoUIParamToolbar) );
    toolbar->setAsToolBar(true);
    toolbar->setEvaluateOnChange(false);
    toolbar->setSecret(true);
    _imp->ui->toolbarPage = toolbar;
    KnobGroupPtr selectionToolButton = createKnob<KnobGroup>(kRotoUIParamSelectionToolButton);
    selectionToolButton->setLabel(tr(kRotoUIParamSelectionToolButtonLabel));
    selectionToolButton->setAsToolButton(true);
    selectionToolButton->setEvaluateOnChange(false);
    selectionToolButton->setSecret(true);
    selectionToolButton->setInViewerContextCanHaveShortcut(true);
    selectionToolButton->setIsPersistent(false);
    toolbar->addKnob(selectionToolButton);
    _imp->ui->selectToolGroup = selectionToolButton;
    KnobGroupPtr editPointsToolButton = createKnob<KnobGroup>(kRotoUIParamEditPointsToolButton);
    editPointsToolButton->setLabel(tr(kRotoUIParamEditPointsToolButtonLabel));
    editPointsToolButton->setAsToolButton(true);
    editPointsToolButton->setEvaluateOnChange(false);
    editPointsToolButton->setSecret(true);
    editPointsToolButton->setInViewerContextCanHaveShortcut(true);
    editPointsToolButton->setIsPersistent(false);
    toolbar->addKnob(editPointsToolButton);
    _imp->ui->pointsEditionToolGroup = editPointsToolButton;
    KnobGroupPtr editBezierToolButton = createKnob<KnobGroup>(kRotoUIParamBezierEditionToolButton);
    editBezierToolButton->setLabel(tr(kRotoUIParamBezierEditionToolButtonLabel));
    editBezierToolButton->setAsToolButton(true);
    editBezierToolButton->setEvaluateOnChange(false);
    editBezierToolButton->setSecret(true);
    editBezierToolButton->setInViewerContextCanHaveShortcut(true);
    editBezierToolButton->setIsPersistent(false);
    toolbar->addKnob(editBezierToolButton);
    _imp->ui->bezierEditionToolGroup = editBezierToolButton;
    KnobGroupPtr paintToolButton = createKnob<KnobGroup>(kRotoUIParamPaintBrushToolButton);
    paintToolButton->setLabel(tr(kRotoUIParamPaintBrushToolButtonLabel));
    paintToolButton->setAsToolButton(true);
    paintToolButton->setEvaluateOnChange(false);
    paintToolButton->setSecret(true);
    paintToolButton->setInViewerContextCanHaveShortcut(true);
    paintToolButton->setIsPersistent(false);
    toolbar->addKnob(paintToolButton);
    _imp->ui->paintBrushToolGroup = paintToolButton;

    KnobGroupPtr cloneToolButton, effectToolButton, mergeToolButton;
    if (_imp->nodeType == eRotoPaintTypeRotoPaint) {
        cloneToolButton = createKnob<KnobGroup>(kRotoUIParamCloneBrushToolButton);
        cloneToolButton->setLabel(tr(kRotoUIParamCloneBrushToolButtonLabel));
        cloneToolButton->setAsToolButton(true);
        cloneToolButton->setEvaluateOnChange(false);
        cloneToolButton->setSecret(true);
        cloneToolButton->setInViewerContextCanHaveShortcut(true);
        cloneToolButton->setIsPersistent(false);
        toolbar->addKnob(cloneToolButton);
        _imp->ui->cloneBrushToolGroup = cloneToolButton;
        effectToolButton = createKnob<KnobGroup>(kRotoUIParamEffectBrushToolButton);
        effectToolButton->setLabel(tr(kRotoUIParamEffectBrushToolButtonLabel));
        effectToolButton->setAsToolButton(true);
        effectToolButton->setEvaluateOnChange(false);
        effectToolButton->setSecret(true);
        effectToolButton->setInViewerContextCanHaveShortcut(true);
        effectToolButton->setIsPersistent(false);
        toolbar->addKnob(effectToolButton);
        _imp->ui->effectBrushToolGroup = effectToolButton;
        mergeToolButton = createKnob<KnobGroup>(kRotoUIParamMergeBrushToolButton);
        mergeToolButton->setLabel(tr(kRotoUIParamMergeBrushToolButtonLabel));
        mergeToolButton->setAsToolButton(true);
        mergeToolButton->setEvaluateOnChange(false);
        mergeToolButton->setSecret(true);
        mergeToolButton->setInViewerContextCanHaveShortcut(true);
        mergeToolButton->setIsPersistent(false);
        toolbar->addKnob(mergeToolButton);
        _imp->ui->mergeBrushToolGroup = mergeToolButton;
    }


    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamSelectAllToolButtonAction);
        tool->setLabel(tr(kRotoUIParamSelectAllToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamSelectAllToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(true);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/cursor.png");
        tool->setIsPersistent(false);
        selectionToolButton->addKnob(tool);
        _imp->ui->selectAllAction = tool;
    }
    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamSelectPointsToolButtonAction);
        tool->setLabel(tr(kRotoUIParamSelectPointsToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamSelectPointsToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/selectPoints.png");
        tool->setIsPersistent(false);
        selectionToolButton->addKnob(tool);
        _imp->ui->selectPointsAction = tool;
    }
    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamSelectShapesToolButtonAction);
        tool->setLabel(tr(kRotoUIParamSelectShapesToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamSelectShapesToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/selectCurves.png");
        tool->setIsPersistent(false);
        selectionToolButton->addKnob(tool);
        _imp->ui->selectCurvesAction = tool;
    }
    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamSelectFeatherPointsToolButtonAction);
        tool->setLabel(tr(kRotoUIParamSelectFeatherPointsToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamSelectFeatherPointsToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/selectFeather.png");
        tool->setIsPersistent(false);
        selectionToolButton->addKnob(tool);
        _imp->ui->selectFeatherPointsAction = tool;
    }
    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamAddPointsToolButtonAction);
        tool->setLabel(tr(kRotoUIParamAddPointsToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamAddPointsToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(true);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/addPoints.png");
        tool->setIsPersistent(false);
        editPointsToolButton->addKnob(tool);
        _imp->ui->addPointsAction = tool;
    }
    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamRemovePointsToolButtonAction);
        tool->setLabel(tr(kRotoUIParamRemovePointsToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamRemovePointsToolButtonAction) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/removePoints.png");
        tool->setIsPersistent(false);
        editPointsToolButton->addKnob(tool);
        _imp->ui->removePointsAction = tool;
    }
    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamCuspPointsToolButtonAction);
        tool->setLabel(tr(kRotoUIParamCuspPointsToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamCuspPointsToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/cuspPoints.png");
        tool->setIsPersistent(false);
        editPointsToolButton->addKnob(tool);
        _imp->ui->cuspPointsAction = tool;
    }
    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamSmoothPointsToolButtonAction);
        tool->setLabel(tr(kRotoUIParamSmoothPointsToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamSmoothPointsToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/smoothPoints.png");
        tool->setIsPersistent(false);
        editPointsToolButton->addKnob(tool);
        _imp->ui->smoothPointsAction = tool;
    }
    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamOpenCloseCurveToolButtonAction);
        tool->setLabel(tr(kRotoUIParamOpenCloseCurveToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamOpenCloseCurveToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/openCloseCurve.png");
        tool->setIsPersistent(false);
        editPointsToolButton->addKnob(tool);
        _imp->ui->openCloseCurveAction = tool;
    }

    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamRemoveFeatherToolButtonAction);
        tool->setLabel(tr(kRotoUIParamRemoveFeatherToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamRemoveFeatherToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/removeFeather.png");
        tool->setIsPersistent(false);
        editPointsToolButton->addKnob(tool);
        _imp->ui->removeFeatherAction = tool;
    }

    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamDrawBezierToolButtonAction);
        tool->setLabel(tr(kRotoUIParamDrawBezierToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamDrawBezierToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(true);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/bezier32.png");
        tool->setIsPersistent(false);
        editBezierToolButton->addKnob(tool);
        _imp->ui->drawBezierAction = tool;
    }

    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamDrawEllipseToolButtonAction);
        tool->setLabel(tr(kRotoUIParamDrawEllipseToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamDrawEllipseToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/ellipse.png");
        tool->setIsPersistent(false);
        editBezierToolButton->addKnob(tool);
        _imp->ui->drawEllipseAction = tool;
    }

    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamDrawRectangleToolButtonAction);
        tool->setLabel(tr(kRotoUIParamDrawRectangleToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamDrawRectangleToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/rectangle.png");
        tool->setIsPersistent(false);
        editBezierToolButton->addKnob(tool);
        _imp->ui->drawRectangleAction = tool;
    }
    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamDrawBrushToolButtonAction);
        tool->setLabel(tr(kRotoUIParamDrawBrushToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamDrawBrushToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(true);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/rotopaint_solid.png");
        tool->setIsPersistent(false);
        paintToolButton->addKnob(tool);
        _imp->ui->brushAction = tool;
    }
    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamPencilToolButtonAction);
        tool->setLabel(tr(kRotoUIParamPencilToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamPencilToolButtonAction) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/rotoToolPencil.png");
        tool->setIsPersistent(false);
        paintToolButton->addKnob(tool);
        _imp->ui->pencilAction = tool;
    }

    {
        KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamEraserToolButtonAction);
        tool->setLabel(tr(kRotoUIParamEraserToolButtonActionLabel));
        tool->setHintToolTip( tr(kRotoUIParamEraserToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecret(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel("Images/rotopaint_eraser.png");
        tool->setIsPersistent(false);
        paintToolButton->addKnob(tool);
        _imp->ui->eraserAction = tool;
    }
    if (_imp->nodeType == eRotoPaintTypeRotoPaint) {
        {
            KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamCloneToolButtonAction);
            tool->setLabel(tr(kRotoUIParamCloneToolButtonActionLabel));
            tool->setHintToolTip( tr(kRotoUIParamCloneToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecret(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel("Images/rotopaint_clone.png");
            tool->setIsPersistent(false);
            cloneToolButton->addKnob(tool);
            _imp->ui->cloneAction = tool;
        }
        {
            KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamRevealToolButtonAction);
            tool->setLabel(tr(kRotoUIParamRevealToolButtonActionLabel));
            tool->setHintToolTip( tr(kRotoUIParamRevealToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecret(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel("Images/rotopaint_reveal.png");
            tool->setIsPersistent(false);
            cloneToolButton->addKnob(tool);
            _imp->ui->revealAction = tool;
        }


        {
            KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamBlurToolButtonAction);
            tool->setLabel(tr(kRotoUIParamBlurToolButtonActionLabel));
            tool->setHintToolTip( tr(kRotoUIParamBlurToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecret(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel("Images/rotopaint_blur.png");
            tool->setIsPersistent(false);
            effectToolButton->addKnob(tool);
            _imp->ui->blurAction = tool;
        }
        {
            KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamSmearToolButtonAction);
            tool->setLabel(tr(kRotoUIParamSmearToolButtonActionLabel));
            tool->setHintToolTip( tr(kRotoUIParamSmearToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecret(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel("Images/rotopaint_smear.png");
            tool->setIsPersistent(false);
            effectToolButton->addKnob(tool);
            _imp->ui->smearAction = tool;
        }
        {
            KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamDodgeToolButtonAction);
            tool->setLabel(tr(kRotoUIParamDodgeToolButtonActionLabel));
            tool->setHintToolTip( tr(kRotoUIParamDodgeToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecret(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel("Images/rotopaint_dodge.png");
            tool->setIsPersistent(false);
            mergeToolButton->addKnob(tool);
            _imp->ui->dodgeAction = tool;
        }
        {
            KnobButtonPtr tool = createKnob<KnobButton>(kRotoUIParamBurnToolButtonAction);
            tool->setLabel(tr(kRotoUIParamBurnToolButtonActionLabel));
            tool->setHintToolTip( tr(kRotoUIParamBurnToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecret(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel("Images/rotopaint_burn.png");
            tool->setIsPersistent(false);
            mergeToolButton->addKnob(tool);
            _imp->ui->burnAction = tool;
        }
    } // if (_imp->isPaintByDefault) {

    // Right click menu
    KnobChoicePtr rightClickMenu = createKnob<KnobChoice>(std::string(kRotoUIParamRightClickMenu) );
    rightClickMenu->setSecret(true);
    rightClickMenu->setEvaluateOnChange(false);
    generalPage->addKnob(rightClickMenu);
    _imp->ui->rightClickMenuKnob = rightClickMenu;
    {
        KnobButtonPtr action = createKnob<KnobButton>(kRotoUIParamRightClickMenuActionRemoveItems);
        action->setLabel(tr(kRotoUIParamRightClickMenuActionRemoveItemsLabel));
        action->setSecret(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        addOverlaySlaveParam(action);
        generalPage->addKnob(action);
        _imp->ui->removeItemsMenuAction = action;
    }
    {
        KnobButtonPtr action = createKnob<KnobButton>(kRotoUIParamRightClickMenuActionCuspItems);
        action->setLabel(tr(kRotoUIParamRightClickMenuActionCuspItemsLabel));
        action->setEvaluateOnChange(false);
        action->setSecret(true);
        action->setInViewerContextCanHaveShortcut(true);
        addOverlaySlaveParam(action);
        generalPage->addKnob(action);
        _imp->ui->cuspItemMenuAction = action;
    }
    {
        KnobButtonPtr action = createKnob<KnobButton>(kRotoUIParamRightClickMenuActionSmoothItems);
        action->setLabel(tr(kRotoUIParamRightClickMenuActionSmoothItemsLabel));
        action->setSecret(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        addOverlaySlaveParam(action);
        generalPage->addKnob(action);
        _imp->ui->smoothItemMenuAction = action;
    }
    {
        KnobButtonPtr action = createKnob<KnobButton>(kRotoUIParamRightClickMenuActionRemoveItemsFeather);
        action->setLabel(tr(kRotoUIParamRightClickMenuActionRemoveItemsFeatherLabel));
        action->setSecret(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        addOverlaySlaveParam(action);
        generalPage->addKnob(action);
        _imp->ui->removeItemFeatherMenuAction = action;
    }
    {
        KnobButtonPtr action = createKnob<KnobButton>(kRotoUIParamRightClickMenuActionNudgeBottom);
        action->setLabel(tr(kRotoUIParamRightClickMenuActionNudgeBottomLabel));
        action->setSecret(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        addOverlaySlaveParam(action);
        generalPage->addKnob(action);
        _imp->ui->nudgeBottomMenuAction = action;
    }
    {
        KnobButtonPtr action = createKnob<KnobButton>(kRotoUIParamRightClickMenuActionNudgeLeft);
        action->setLabel(tr(kRotoUIParamRightClickMenuActionNudgeLeftLabel));
        action->setSecret(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        addOverlaySlaveParam(action);
        generalPage->addKnob(action);
        _imp->ui->nudgeLeftMenuAction = action;
    }
    {
        KnobButtonPtr action = createKnob<KnobButton>(kRotoUIParamRightClickMenuActionNudgeTop);
        action->setLabel(tr(kRotoUIParamRightClickMenuActionNudgeTopLabel));
        action->setSecret(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        addOverlaySlaveParam(action);
        generalPage->addKnob(action);
        _imp->ui->nudgeTopMenuAction = action;
    }
    {
        KnobButtonPtr action = createKnob<KnobButton>(kRotoUIParamRightClickMenuActionNudgeRight);
        action->setLabel(tr(kRotoUIParamRightClickMenuActionNudgeRightLabel));
        action->setSecret(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        addOverlaySlaveParam(action);
        generalPage->addKnob(action);
        _imp->ui->nudgeRightMenuAction = action;
    }
    {
        KnobButtonPtr action = createKnob<KnobButton>(kRotoUIParamRightClickMenuActionSelectAll);
        action->setLabel(tr(kRotoUIParamRightClickMenuActionSelectAllLabel));
        action->setSecret(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        generalPage->addKnob(action);
        _imp->ui->selectAllMenuAction = action;
    }
    {
        KnobButtonPtr action = createKnob<KnobButton>(kRotoUIParamRightClickMenuActionOpenClose);
        action->setLabel(tr(kRotoUIParamRightClickMenuActionOpenCloseLabel));
        action->setSecret(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        addOverlaySlaveParam(action);
        generalPage->addKnob(action);
        _imp->ui->openCloseMenuAction = action;
    }
    {
        KnobButtonPtr action = createKnob<KnobButton>(kRotoUIParamRightClickMenuActionLockShapes);
        action->setLabel(tr(kRotoUIParamRightClickMenuActionLockShapesLabel));
        action->setSecret(true);
        action->setEvaluateOnChange(false);
        action->setInViewerContextCanHaveShortcut(true);
        addOverlaySlaveParam(action);
        generalPage->addKnob(action);
        _imp->ui->lockShapeMenuAction = action;
    }

    KnobButtonPtr defaultAction;
    KnobGroupPtr defaultRole;
    if (_imp->nodeType == eRotoPaintTypeRotoPaint) {
        defaultAction = _imp->ui->brushAction.lock();
        defaultRole = _imp->ui->paintBrushToolGroup.lock();
    } else {
        defaultAction = _imp->ui->drawBezierAction.lock();
        defaultRole = _imp->ui->bezierEditionToolGroup.lock();
    }
    _imp->ui->setCurrentTool(defaultAction);
    _imp->ui->onRoleChangedInternal(defaultRole);
    _imp->ui->onToolChangedInternal(defaultAction);
} // initViewerUIKnobs

void
RotoPaint::initializeKnobs()
{
    BlockTreeRefreshRAII preventTreeRefresh(_imp.get());

    RotoPaintPtr thisShared = toRotoPaint(shared_from_this());

    _imp->knobsTable.reset(new RotoPaintKnobItemsTable(_imp.get(), KnobItemsTable::eKnobItemsTableTypeTree));
    _imp->knobsTable->setIconsPath(NATRON_IMAGES_PATH);

    QObject::connect( _imp->knobsTable.get(), SIGNAL(selectionChanged(std::list<KnobTableItemPtr>,std::list<KnobTableItemPtr>,TableChangeReasonEnum)), this, SLOT(onModelSelectionChanged(std::list<KnobTableItemPtr>,std::list<KnobTableItemPtr>,TableChangeReasonEnum)) );

    KnobPagePtr generalPage = getOrCreateKnob<KnobPage>(kRotoPaintGeneralPageParam);
    generalPage->setLabel(tr(kRotoPaintGeneralPageParamLabel));
    assert(generalPage);

    if (_imp->nodeType == eRotoPaintTypeComp) {
        initCompNodeKnobs(generalPage);
    } else {
        initGeneralPageKnobs();
        initShapePageKnobs();
        initStrokePageKnobs();
        initTransformPageKnobs();
        initClonePageKnobs();
        initMotionBlurPageKnobs();
    }

    setItemsTable(_imp->knobsTable, kRotoPaintGeneralPageParam);




    if (_imp->nodeType != eRotoPaintTypeComp) {

        // The mix knob is per-item
        {
            KnobDoublePtr mixKnob = getOrCreateHostMixKnob(generalPage);
            _imp->knobsTable->addPerItemKnobMaster(mixKnob);
        }

        KnobSeparatorPtr sep = createKnob<KnobSeparator>("outputSeparator");
        sep->setLabel(tr("Output"));
        generalPage->addKnob(sep);
    }


    std::string channelNames[4] = {"doRed", "doGreen", "doBlue", "doAlpha"};
    std::string channelLabels[4] = {"R", "G", "B", "A"};
    bool defaultValues[4];
    bool channelSelectorSupported = isHostChannelSelectorSupported(&defaultValues[0], &defaultValues[1], &defaultValues[2], &defaultValues[3]);
    Q_UNUSED(channelSelectorSupported);

    for (int i = 0; i < 4; ++i) {
        KnobBoolPtr enabled =  createKnob<KnobBool>(channelNames[i]);
        enabled->setLabel(channelLabels[i]);
        enabled->setAnimationEnabled(false);
        enabled->setAddNewLine(i == 3);
        enabled->setDefaultValue(defaultValues[i]);
        enabled->setHintToolTip( tr("Enable drawing onto this channel") );
        if (_imp->nodeType == eRotoPaintTypeComp) {
            // For comp node, insert checkboxes on top
            generalPage->insertKnob(i, enabled);
        } else {
            generalPage->addKnob(enabled);
        }
        _imp->enabledKnobs[i] = enabled;
    }


    if (_imp->nodeType != eRotoPaintTypeComp) {
        KnobBoolPtr premultKnob = createKnob<KnobBool>("premultiply");
        premultKnob->setLabel(tr("Premultiply"));
        premultKnob->setHintToolTip( tr("When checked, the red, green and blue channels of the output are premultiplied by the alpha channel.\n"
                                        "This will result in the pixels outside of the shapes and paint strokes being black and transparent.\n"
                                        "This should only be used if all the inputs are Opaque or UnPremultiplied, and only the Alpha channel "
                                        "is selected to be drawn by this node.") );
        premultKnob->setDefaultValue(false);
        premultKnob->setAnimationEnabled(false);
        premultKnob->setIsMetadataSlave(true);
        _imp->premultKnob = premultKnob;
        generalPage->addKnob(premultKnob);
    }

    if (_imp->nodeType != eRotoPaintTypeComp) {
        initViewerUIKnobs(generalPage);
    }

    if (_imp->nodeType != eRotoPaintTypeComp) {
        _imp->knobsTable->setColumnText(0, tr("Label").toStdString());
        _imp->knobsTable->setColumnIcon(1, "visible.png");
        _imp->knobsTable->setColumnTooltip(1, kParamRotoItemEnabledHint);
        _imp->knobsTable->setColumnIcon(2, "locked.png");
        _imp->knobsTable->setColumnTooltip(2, kParamRotoItemLockedHint);
        _imp->knobsTable->setColumnIcon(3, "roto_merge.png");
        _imp->knobsTable->setColumnTooltip(3, kRotoCompOperatorHint);
        _imp->knobsTable->setColumnIcon(4, "colorwheel_overlay.png");
        _imp->knobsTable->setColumnTooltip(4, kRotoOverlayColorHint);
        _imp->knobsTable->setColumnIcon(5, "colorwheel.png");
        _imp->knobsTable->setColumnTooltip(5, kRotoColorHint);
        (void)getOrCreateBaseLayer();
    } else {
        _imp->knobsTable->setColumnText(0, tr("Label").toStdString());
        _imp->knobsTable->setColumnIcon(1, "visible.png");
        _imp->knobsTable->setColumnTooltip(1, kParamRotoItemEnabledHint);
        _imp->knobsTable->setColumnIcon(2, "soloOff.png");
        _imp->knobsTable->setColumnTooltip(2, kParamRotoItemSoloHint);
        _imp->knobsTable->setColumnIcon(3, "roto_merge.png");
        _imp->knobsTable->setColumnTooltip(3, kRotoCompOperatorHint);
        _imp->knobsTable->setColumnIcon(4, "mix.png");
        _imp->knobsTable->setColumnTooltip(4, kLayeredCompMixParamHint);
        _imp->knobsTable->setColumnIcon(5, "lifetime.png");
        _imp->knobsTable->setColumnTooltip(5, kRotoDrawableItemLifeTimeParamHint);
        _imp->knobsTable->setColumnIcon(6, "timeOffset.png");
        _imp->knobsTable->setColumnTooltip(6, kRotoBrushTimeOffsetParamHint_Comp);
        _imp->knobsTable->setColumnIcon(7, "source.png");
        _imp->knobsTable->setColumnTooltip(7, kRotoDrawableItemMergeAInputParamHint_CompNode);
        _imp->knobsTable->setColumnIcon(8, "uninverted.png");
        _imp->knobsTable->setColumnTooltip(8, kRotoInvertedHint);
        _imp->knobsTable->setColumnIcon(9, "maskOff.png");
        _imp->knobsTable->setColumnTooltip(9, kRotoDrawableItemMergeMaskParamHint);
    }

    _imp->refreshSourceKnobs();


} // RotoPaint::initializeKnobs

void
RotoPaint::fetchRenderCloneKnobs()
{
    EffectInstance::fetchRenderCloneKnobs();
    _imp->motionBlurTypeKnob = toKnobChoice(getKnobByName(kRotoMotionBlurModeParam));
    _imp->globalMotionBlurKnob = toKnobInt(getKnobByName(kRotoGlobalMotionBlurParam));
    _imp->globalShutterKnob = toKnobDouble(getKnobByName(kRotoGlobalShutterParam));
    _imp->globalShutterTypeKnob = toKnobChoice(getKnobByName(kRotoGlobalShutterOffsetTypeParam));
    _imp->globalCustomOffsetKnob = toKnobDouble(getKnobByName(kRotoGlobalShutterCustomOffsetParam));

} // fetchRenderCloneKnobs

bool
RotoPaint::shouldPreferPluginOverlayOverHostOverlay() const
{
    return !_imp->ui->ctrlDown;
}

bool
RotoPaint::shouldDrawHostOverlay() const
{
    KnobButtonPtr b = _imp->ui->showTransformHandle.lock();

    if (!b) {
        return true;
    }
    return b->getValue();
}

void
RotoPaint::onKnobsLoaded()
{

    // When loading a roto node, always switch to selection mode by default
    KnobButtonPtr defaultAction = _imp->ui->selectAllAction.lock();
    KnobGroupPtr defaultRole = _imp->ui->selectToolGroup.lock();;

    if (defaultAction && defaultRole) {
        _imp->ui->setCurrentTool(defaultAction);
        _imp->ui->onRoleChangedInternal(defaultRole);
        _imp->ui->onToolChangedInternal(defaultAction);

    }

    _imp->refreshMotionBlurKnobsVisibility();

    _imp->refreshSourceKnobs();

    // Refresh solo items
    std::list<RotoDrawableItemPtr> allItems;
    _imp->knobsTable->getRotoItemsByRenderOrder(TimeValue(0), ViewIdx(0), false /*onlyActives*/);
    for (std::list<RotoDrawableItemPtr>::const_iterator it = allItems.begin(); it != allItems.end(); ++it) {
        if ((*it)->getSoloKnob()->getValue()) {
            _imp->soloItems.insert(*it);
        }
    }
}

bool
RotoPaint::knobChanged(const KnobIPtr& k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSetSpec view,
                       TimeValue time)
{
    if (!k) {
        return false;
    }
    ViewIdx view_i(0);
    if (view.isViewIdx()) {
        view_i = ViewIdx(view);
    }

    bool ret = true;
    KnobIPtr kShared = k->shared_from_this();
    KnobButtonPtr isBtn = toKnobButton(kShared);
    KnobGroupPtr isGrp = toKnobGroup(kShared);
    if ( isBtn && _imp->ui->onToolChangedInternal(isBtn) ) {
        return true;
    } else if ( isGrp && _imp->ui->onRoleChangedInternal(isGrp) ) {
        return true;
    } else if ( k == _imp->ui->colorWheelButton.lock() ) {
        _imp->ui->onBreakMultiStrokeTriggered();
    } else if ( k == _imp->ui->pressureOpacityButton.lock() ) {
        _imp->ui->onBreakMultiStrokeTriggered();
    } else if ( k == _imp->ui->pressureSizeButton.lock() ) {
        _imp->ui->onBreakMultiStrokeTriggered();
    } else if ( k == _imp->ui->hardnessSpinbox.lock() ) {
        _imp->ui->onBreakMultiStrokeTriggered();
    } else if ( k == _imp->ui->buildUpButton.lock() ) {
        _imp->ui->onBreakMultiStrokeTriggered();
    } else if ( k == _imp->ui->resetCloneOffsetButton.lock() ) {
        _imp->ui->onBreakMultiStrokeTriggered();
        _imp->ui->cloneOffset.first = _imp->ui->cloneOffset.second = 0;
    } else if ( k == _imp->ui->addKeyframeButton.lock() ) {
        std::list<KnobTableItemPtr> selection = _imp->knobsTable->getSelectedItems();
        for (std::list<KnobTableItemPtr>::const_iterator it = selection.begin(); it != selection.end(); ++it) {
            BezierPtr isBezier = toBezier(*it);
            if (!isBezier) {
                continue;
            }
            isBezier->setKeyFrame(time, view, 0);
            isBezier->invalidateCacheHashAndEvaluate(true, false);
        }

    } else if ( k == _imp->ui->removeKeyframeButton.lock() ) {
        SelectedItems selection = _imp->knobsTable->getSelectedDrawableItems();
        for (SelectedItems::const_iterator it = selection.begin(); it != selection.end(); ++it) {
            BezierPtr isBezier = toBezier(*it);
            if (!isBezier) {
                continue;
            }
            isBezier->deleteValueAtTime(time, view, DimSpec(0), eValueChangedReasonUserEdited);
        }
    } else if ( k == _imp->ui->removeItemsMenuAction.lock() ) {
        ///if control points are selected, delete them, otherwise delete the selected beziers
        if ( !_imp->ui->selectedCps.empty() ) {
            pushUndoCommand( new RemovePointUndoCommand(_imp->ui, _imp->ui->selectedCps, view_i) );
        } else {
            std::list<KnobTableItemPtr> selection = _imp->knobsTable->getSelectedItems();
            if (selection.empty()) {
                return false;
            } else {
                pushUndoCommand( new RemoveItemsCommand(selection) );
            }
        }
    } else if ( k == _imp->ui->smoothItemMenuAction.lock() ) {
        if ( !_imp->ui->smoothSelectedCurve(time, view_i) ) {
            return false;
        }
    } else if ( k == _imp->ui->cuspItemMenuAction.lock() ) {
        if ( !_imp->ui->cuspSelectedCurve(time, view_i) ) {
            return false;
        }
    } else if ( k == _imp->ui->removeItemFeatherMenuAction.lock() ) {
        if ( !_imp->ui->removeFeatherForSelectedCurve(view_i) ) {
            return false;
        }
    } else if ( k == _imp->ui->nudgeLeftMenuAction.lock() ) {
        if ( !_imp->ui->moveSelectedCpsWithKeyArrows(-1, 0, time, view_i) ) {
            return false;
        }
    } else if ( k == _imp->ui->nudgeRightMenuAction.lock() ) {
        if ( !_imp->ui->moveSelectedCpsWithKeyArrows(1, 0, time, view_i) ) {
            return false;
        }
    } else if ( k == _imp->ui->nudgeBottomMenuAction.lock() ) {
        if ( !_imp->ui->moveSelectedCpsWithKeyArrows(0, -1, time, view_i) ) {
            return false;
        }
    } else if ( k == _imp->ui->nudgeTopMenuAction.lock() ) {
        if ( !_imp->ui->moveSelectedCpsWithKeyArrows(0, 1, time, view_i) ) {
            return false;
        }
    } else if ( k == _imp->ui->selectAllMenuAction.lock() ) {
        _imp->ui->iSelectingwithCtrlA = true;
        ///if no bezier are selected, select all beziers
        SelectedItems selection = _imp->knobsTable->getSelectedDrawableItems();
        if ( selection.empty() ) {
            _imp->knobsTable->selectAll(eTableChangeReasonInternal);
        } else {
            ///select all the control points of all selected beziers
            _imp->ui->selectedCps.clear();
            for (SelectedItems::iterator it = selection.begin(); it != selection.end(); ++it) {
                BezierPtr isBezier = toBezier(*it);
                if (!isBezier) {
                    continue;
                }
                std::list<BezierCPPtr > cps = isBezier->getControlPoints(view_i);
                std::list<BezierCPPtr > fps = isBezier->getFeatherPoints(view_i);
                assert( cps.size() == fps.size() );

                std::list<BezierCPPtr >::const_iterator cpIT = cps.begin();
                for (std::list<BezierCPPtr >::const_iterator fpIT = fps.begin(); fpIT != fps.end(); ++fpIT, ++cpIT) {
                    _imp->ui->selectedCps.push_back( std::make_pair(*cpIT, *fpIT) );
                }
            }
            _imp->ui->computeSelectedCpsBBOX();
        }
    } else if ( k == _imp->ui->openCloseMenuAction.lock() ) {
        if ( ( (_imp->ui->selectedTool == eRotoToolDrawBezier) || (_imp->ui->selectedTool == eRotoToolOpenBezier) ) && _imp->ui->builtBezier && !_imp->ui->builtBezier->isCurveFinished(ViewIdx(0)) ) {
            pushUndoCommand( new OpenCloseUndoCommand(_imp->ui, _imp->ui->builtBezier, view_i) );

            _imp->ui->builtBezier.reset();
            _imp->ui->selectedCps.clear();
            _imp->ui->setCurrentTool( _imp->ui->selectAllAction.lock() );
        }
    } else if ( k == _imp->ui->lockShapeMenuAction.lock() ) {
        _imp->ui->lockSelectedCurves();
    } else if (k == _imp->lifeTimeKnob.lock()) {
        RotoPaintItemLifeTimeTypeEnum lifetime = (RotoPaintItemLifeTimeTypeEnum)_imp->lifeTimeKnob.lock()->getValue();
        _imp->customRangeKnob.lock()->setSecret(lifetime != eRotoPaintItemLifeTimeTypeCustom);
        KnobIntPtr frame = _imp->lifeTimeFrameKnob.lock();
        frame->setSecret(lifetime == eRotoPaintItemLifeTimeTypeCustom || lifetime == eRotoPaintItemLifeTimeTypeAll);
        if (lifetime != eRotoPaintItemLifeTimeTypeCustom) {
            frame->setValue(time);
        }
    } else if ( k == _imp->resetCenterKnob.lock() ) {
        _imp->resetTransformCenter();
    } else if ( k == _imp->resetCloneCenterKnob.lock() ) {
        _imp->resetCloneTransformCenter();
    } else if ( k == _imp->resetTransformKnob.lock() ) {
        _imp->resetTransform();
    } else if ( k == _imp->resetCloneTransformKnob.lock() ) {
        _imp->resetCloneTransform();
    } else if ( k == _imp->motionBlurTypeKnob.lock() ) {
        _imp->refreshMotionBlurKnobsVisibility();
        refreshRotoPaintTree();

    } else if ( k == _imp->removeItemButtonKnob.lock()) {
        std::list<KnobTableItemPtr> selection = _imp->knobsTable->getSelectedItems();
        if (selection.empty()) {
            return false;
        } else {
            pushUndoCommand( new RemoveItemsCommand(selection) );
        }
    } else if ( k == _imp->addGroupButtonKnob.lock()) {
        RotoLayerPtr item = addLayer();
        pushUndoCommand(new AddItemsCommand(item));
    } else if ( k == _imp->addLayerButtonKnob.lock()) {
        CompNodeItemPtr item = makeCompNodeItem();
        pushUndoCommand(new AddItemsCommand(item));
    } else {
        ret = false;
    }

    return ret;
} // RotoPaint::knobChanged

void
RotoPaintPrivate::refreshMotionBlurKnobsVisibility()
{
    RotoMotionBlurModeEnum mbType = (RotoMotionBlurModeEnum)motionBlurTypeKnob.lock()->getValue();
    motionBlurKnob.lock()->setSecret(mbType != eRotoMotionBlurModePerShape);
    shutterKnob.lock()->setSecret(mbType != eRotoMotionBlurModePerShape);
    shutterTypeKnob.lock()->setSecret(mbType != eRotoMotionBlurModePerShape);
    customOffsetKnob.lock()->setSecret(mbType != eRotoMotionBlurModePerShape);

    globalMotionBlurKnob.lock()->setSecret(mbType != eRotoMotionBlurModeGlobal);
    globalShutterKnob.lock()->setSecret(mbType != eRotoMotionBlurModeGlobal);
    globalShutterTypeKnob.lock()->setSecret(mbType != eRotoMotionBlurModeGlobal);
    globalCustomOffsetKnob.lock()->setSecret(mbType != eRotoMotionBlurModeGlobal);
}

void
RotoPaint::refreshExtraStateAfterTimeChanged(bool isPlayback,
                                             TimeValue time)
{
    NodeGroup::refreshExtraStateAfterTimeChanged(isPlayback, time);
    if (time != _imp->ui->strokeBeingPaintedTimelineFrame) {
        if ( (_imp->ui->selectedTool == eRotoToolBlur) ||
            ( _imp->ui->selectedTool == eRotoToolBurn) ||
            ( _imp->ui->selectedTool == eRotoToolDodge) ||
            ( _imp->ui->selectedTool == eRotoToolClone) ||
            ( _imp->ui->selectedTool == eRotoToolEraserBrush) ||
            ( _imp->ui->selectedTool == eRotoToolSolidBrush) ||
            ( _imp->ui->selectedTool == eRotoToolReveal) ||
            ( _imp->ui->selectedTool == eRotoToolSmear) ||
            ( _imp->ui->selectedTool == eRotoToolSharpen) ) {
            _imp->ui->onBreakMultiStrokeTriggered();
        }
    }
    _imp->ui->computeSelectedCpsBBOX();
}

void
RotoPaint::onInputChanged(int inputNb)
{


    _imp->refreshSourceKnobs();

    refreshRotoPaintTree();
    
    NodeGroup::onInputChanged(inputNb);
}


static void
getRotoItemsByRenderOrderInternal(std::list< RotoDrawableItemPtr > * curves,
                                  const KnobTableItemPtr& item,
                                  TimeValue time, ViewIdx view,
                                  bool onlyActives)
{
    RotoDrawableItemPtr isDrawable = boost::dynamic_pointer_cast<RotoDrawableItem>(item);
    if (isDrawable) {
         if ( !onlyActives || isDrawable->isActivated(time, view) ) {
             curves->push_front(isDrawable);
         }
    }

    std::vector<KnobTableItemPtr> children = item->getChildren();

    for (std::vector<KnobTableItemPtr>::const_iterator it = children.begin(); it != children.end(); ++it) {
        getRotoItemsByRenderOrderInternal(curves, *it, time, view, onlyActives);
    }
}

std::list< RotoDrawableItemPtr >
RotoPaintKnobItemsTable::getRotoItemsByRenderOrder(TimeValue time, ViewIdx view, bool onlyActivated) const
{
    std::list< RotoDrawableItemPtr > ret;
    std::vector<KnobTableItemPtr> topLevelItems = getTopLevelItems();

    // Roto should have only a single top level layer
    if (topLevelItems.size() == 0) {
        return ret;
    }
    for (std::vector<KnobTableItemPtr>::const_iterator it = topLevelItems.begin(); it!=topLevelItems.end(); ++it) {
        getRotoItemsByRenderOrderInternal(&ret, *it, time, view, onlyActivated);
    }
    return ret;
}

SelectedItems
RotoPaintKnobItemsTable::getSelectedDrawableItems() const
{
    SelectedItems drawables;
    std::list<KnobTableItemPtr> selection = getSelectedItems();
    for (std::list<KnobTableItemPtr>::const_iterator it = selection.begin(); it != selection.end(); ++it) {
        RotoDrawableItemPtr drawable = boost::dynamic_pointer_cast<RotoDrawableItem>(*it);
        if (drawable) {
            drawables.push_back(drawable);
        }
    }
    return drawables;
}

void
RotoPaintKnobItemsTable::fromSerialization(const SERIALIZATION_NAMESPACE::SerializationObjectBase & obj)
{
    BlockTreeRefreshRAII preventTreeRefresh(_imp);
    KnobItemsTable::fromSerialization(obj);
    _imp->publicInterface->refreshRotoPaintTree();
}


void
RotoPaintKnobItemsTable::onModelReset()
{
    if (_imp->nodeType != RotoPaint::eRotoPaintTypeComp) {
        _imp->createBaseLayer();
    }
}


void
RotoPaintPrivate::resetTransformsCenter(bool doClone,
                                   bool doTransform)
{
    TimeValue time(publicInterface->getApp()->getTimeLine()->currentFrame());
    ViewIdx view(0);
    RectD bbox;
    {
        bool bboxSet = false;
        SelectedItems selection = knobsTable->getSelectedDrawableItems();
        if (selection.empty()) {
            selection = knobsTable->getRotoItemsByRenderOrder(time, view);
        }
        for (SelectedItems::const_iterator it = selection.begin(); it!=selection.end(); ++it) {
            RotoDrawableItemPtr drawable = boost::dynamic_pointer_cast<RotoDrawableItem>(*it);
            if (!drawable) {
                continue;
            }
            RectD thisShapeBox = drawable->getBoundingBox(time, view);
            if (thisShapeBox.isNull()) {
                continue;
            }
            if (!bboxSet) {
                bbox = thisShapeBox;
            } else {
                bbox.merge(thisShapeBox);
            }
        }
    }
    std::vector<double> values(2);
    values[0] = (bbox.x1 + bbox.x2) / 2.;
    values[1] = (bbox.y1 + bbox.y2) / 2.;
    
    //getItemsRegionOfDefinition(knobsTable->getSelectedItems(), time, ViewIdx(0), &bbox);
    if (doTransform) {
        KnobDoublePtr center = centerKnob.lock();
        center->beginChanges();
        center->removeAnimation(ViewSetSpec::all(), DimSpec::all(), eValueChangedReasonUserEdited);
        center->setValueAcrossDimensions(values);
        center->endChanges();
    }
    if (doClone) {
        KnobDoublePtr centerKnob = cloneCenterKnob.lock();
        centerKnob->beginChanges();
        centerKnob->removeAnimation(ViewSetSpec::all(), DimSpec::all(), eValueChangedReasonUserEdited);
        centerKnob->setValueAcrossDimensions(values);
        centerKnob->endChanges();
    }
}

void
RotoPaintPrivate::resetTransformCenter()
{
    resetTransformsCenter(false, true);
}

void
RotoPaintPrivate::resetCloneTransformCenter()
{
    resetTransformsCenter(true, false);
}

void
RotoPaintPrivate::resetTransformInternal(const KnobDoublePtr& translate,
                                    const KnobDoublePtr& scale,
                                    const KnobDoublePtr& center,
                                    const KnobDoublePtr& rotate,
                                    const KnobDoublePtr& skewX,
                                    const KnobDoublePtr& skewY,
                                    const KnobBoolPtr& scaleUniform,
                                    const KnobChoicePtr& skewOrder,
                                    const KnobDoublePtr& extraMatrix)
{
    std::list<KnobIPtr> knobs;

    knobs.push_back(translate);
    knobs.push_back(scale);
    knobs.push_back(center);
    knobs.push_back(rotate);
    knobs.push_back(skewX);
    knobs.push_back(skewY);
    knobs.push_back(scaleUniform);
    knobs.push_back(skewOrder);
    if (extraMatrix) {
        knobs.push_back(extraMatrix);
    }
    bool wasEnabled = translate->isEnabled();
    for (std::list<KnobIPtr>::iterator it = knobs.begin(); it != knobs.end(); ++it) {
        (*it)->resetToDefaultValue(DimSpec::all(), ViewSetSpec::all());
        (*it)->setEnabled(wasEnabled);
    }
}

void
RotoPaintPrivate::resetTransform()
{
    KnobDoublePtr translate = translateKnob.lock();
    KnobDoublePtr center = centerKnob.lock();
    KnobDoublePtr scale = scaleKnob.lock();
    KnobDoublePtr rotate = rotateKnob.lock();
    KnobBoolPtr uniform = scaleUniformKnob.lock();
    KnobDoublePtr skewX = skewXKnob.lock();
    KnobDoublePtr skewY = skewYKnob.lock();
    KnobChoicePtr skewOrder = skewOrderKnob.lock();
    KnobDoublePtr extraMatrix = extraMatrixKnob.lock();

    resetTransformInternal(translate, scale, center, rotate, skewX, skewY, uniform, skewOrder, extraMatrix);
}

void
RotoPaintPrivate::resetCloneTransform()
{
    KnobDoublePtr translate = cloneTranslateKnob.lock();
    KnobDoublePtr center = cloneCenterKnob.lock();
    KnobDoublePtr scale = cloneScaleKnob.lock();
    KnobDoublePtr rotate = cloneRotateKnob.lock();
    KnobBoolPtr uniform = cloneUniformKnob.lock();
    KnobDoublePtr skewX = cloneSkewXKnob.lock();
    KnobDoublePtr skewY = cloneSkewYKnob.lock();
    KnobChoicePtr skewOrder = cloneSkewOrderKnob.lock();

    resetTransformInternal( translate, scale, center, rotate, skewX, skewY, uniform, skewOrder, KnobDoublePtr() );
}

void
RotoPaint::onBreakMultiStrokeTriggered()
{
    _imp->ui->onBreakMultiStrokeTriggered();
}

void
RotoPaint::onEnableOpenGLKnobValueChanged(bool /*activated*/)
{
    _imp->ui->onBreakMultiStrokeTriggered();
}

void
RotoPaint::onModelSelectionChanged(std::list<KnobTableItemPtr> /*addedToSelection*/, std::list<KnobTableItemPtr> /*removedFromSelection*/, TableChangeReasonEnum /*reason*/)
{
    requestOverlayInteractRefresh();
}


KnobTableItemPtr
RotoPaintKnobItemsTable::createItemFromSerialization(const SERIALIZATION_NAMESPACE::KnobTableItemSerializationPtr& data)
{
    SERIALIZATION_NAMESPACE::BezierSerializationPtr isBezier = boost::dynamic_pointer_cast<SERIALIZATION_NAMESPACE::BezierSerialization>(data);
    if (isBezier) {
        BezierPtr ret(new Bezier(_imp->knobsTable, std::string(), isBezier->_isOpenBezier));
        ret->initializeKnobsPublic();
        ret->fromSerialization(*isBezier);
        return ret;
    }
    SERIALIZATION_NAMESPACE::RotoStrokeItemSerializationPtr isStroke = boost::dynamic_pointer_cast<SERIALIZATION_NAMESPACE::RotoStrokeItemSerialization>(data);
    if (isStroke) {
        RotoStrokeItemPtr ret(new RotoStrokeItem(RotoStrokeItem::strokeTypeFromSerializationString(isStroke->verbatimTag), _imp->knobsTable));
        ret->initializeKnobsPublic();
        ret->fromSerialization(*isStroke);
        return ret;
    }

    // By default, assume this is a layer
    if (data->verbatimTag == kSerializationRotoGroupTag) {
        RotoLayerPtr ret(new RotoLayer(_imp->knobsTable));
        ret->initializeKnobsPublic();
        ret->fromSerialization(*data);
        return ret;
    } else if (data->verbatimTag == kSerializationCompLayerTag) {
        CompNodeItemPtr ret(new CompNodeItem(_imp->knobsTable));
        ret->initializeKnobsPublic();
        ret->fromSerialization(*data);
        return ret;
    }
    return KnobTableItemPtr();
    
}


SERIALIZATION_NAMESPACE::KnobTableItemSerializationPtr
RotoPaintKnobItemsTable::createSerializationFromItem(const KnobTableItemPtr& item)
{
    RotoLayerPtr isLayer = toRotoLayer(item);
    BezierPtr isBezier = toBezier(item);
    RotoStrokeItemPtr isStroke = toRotoStrokeItem(item);
    CompNodeItemPtr compItem = toCompNodeItem(item);
    if (isLayer || compItem) {
        SERIALIZATION_NAMESPACE::KnobTableItemSerializationPtr ret(new SERIALIZATION_NAMESPACE::KnobTableItemSerialization);
        item->toSerialization(ret.get());
        return ret;
    } else if (isBezier) {
        SERIALIZATION_NAMESPACE::BezierSerializationPtr ret(new SERIALIZATION_NAMESPACE::BezierSerialization(isBezier->isOpenBezier()));
        isBezier->toSerialization(ret.get());
        return ret;
    } else if (isStroke) {
        SERIALIZATION_NAMESPACE::RotoStrokeItemSerializationPtr ret(new SERIALIZATION_NAMESPACE::RotoStrokeItemSerialization);
        isStroke->toSerialization(ret.get());
        return ret;
    }
    return KnobItemsTable::createSerializationFromItem(item);
}

bool
RotoPaintPrivate::isRotoPaintTreeConcatenatableInternal(const std::list<RotoDrawableItemPtr >& items,  int* blendingMode) const
{
    // Iterate over items, if they all have the same compositing operator, concatenate. Concatenation only works for Solids or Comp items. If a comp item has a mask or mix is animated or different than 1, we cannot concatenate
    bool operatorSet = false;
    int comp_i = -1;

    for (std::list<RotoDrawableItemPtr >::const_iterator it = items.begin(); it != items.end(); ++it) {

        MergingFunctionEnum op = (MergingFunctionEnum)(*it)->getOperatorKnob()->getValue();

        // Can only concatenate with over
        if (op != eMergeOver) {
            return false;
        }

        if (!operatorSet) {
            operatorSet = true;
            comp_i = op;
        } else {
            if (op != comp_i) {
                // 2 items have a different compositing operator
                return false;
            }
        }

        RotoPaintItemLifeTimeTypeEnum lifeTime = (RotoPaintItemLifeTimeTypeEnum)(*it)->getLifeTimeFrameKnob()->getValue();
        if (lifeTime != eRotoPaintItemLifeTimeTypeAll && lifeTime != eRotoPaintItemLifeTimeTypeCustom) {
            // An item with a varying lifetime makes the concatenation impossible: we cannot disconenct and reconnect the A input of the global
            // Merge through time.
            return false;
        }
        if (lifeTime == eRotoPaintItemLifeTimeTypeCustom) {
            // If custom and the custom range checkbox is animated or unchecked, do not concatenate
            KnobBoolPtr customRange = (*it)->getCustomRangeKnob();
            if (customRange->hasAnimation() || !customRange->getValue()) {
                return false;
            }
        }

        // Now check the global activated/solo switches
        if (!(*it)->isGloballyActivated()) {
            return false;
        }

        RotoStrokeType type = (*it)->getBrushType();

        // Other item types cannot concatenate since they use a custom mask on their Merge node.
        if (type != eRotoStrokeTypeSolid && type != eRotoStrokeTypeComp) {
            return false;
        }


        // If the comp item has a mask on the merge node or a mix != 1, forget concatenating
        if (type == eRotoStrokeTypeComp) {
            if ((*it)->getMergeMaskChoiceKnob()->getValue() > 0) {
                return false;
            }
            KnobDoublePtr mixKnob = (*it)->getMixKnob();
            if (mixKnob->hasAnimation() || mixKnob->getValue() != 1.) {
                return false;
            }

        }

        
    }
    if (operatorSet) {
        *blendingMode = comp_i;

        return true;
    }

    return false;
} // isRotoPaintTreeConcatenatableInternal

bool
RotoPaint::isRotoPaintTreeConcatenatable() const
{
    std::list<RotoDrawableItemPtr > items = _imp->knobsTable->getRotoItemsByRenderOrder(getCurrentTime_TLS(), getCurrentView_TLS(), false);
    int bop;
    return _imp->isRotoPaintTreeConcatenatableInternal(items, &bop);
}


static void setOperationKnob(const NodePtr& node, int blendingOperator)
{
    KnobIPtr mergeOperatorKnob = node->getKnobByName(kMergeOFXParamOperation);
    KnobChoicePtr mergeOp = toKnobChoice( mergeOperatorKnob );
    if (mergeOp) {
        mergeOp->setValue(blendingOperator);
    }
}

NodePtr
RotoPaintPrivate::getOrCreateGlobalTimeBlurNode()
{
    if (globalTimeBlurNode) {
        return globalTimeBlurNode;
    }
    NodePtr node = publicInterface->getNode();
    RotoPaintPtr rotoPaintEffect = toRotoPaint(node->getEffectInstance());

    CreateNodeArgsPtr args(CreateNodeArgs::create( PLUGINID_OFX_TIMEBLUR, rotoPaintEffect ));
    args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
    args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
    args->setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
    args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, "GlobalMotionBlur");
    globalTimeBlurNode = node->getApp()->createNode(args);
    assert(globalTimeBlurNode);
    if (!globalTimeBlurNode) {
        throw std::runtime_error(publicInterface->tr("Rotopaint requires the plug-in %1 in order to work").arg(QLatin1String(PLUGINID_OFX_TIMEBLUR)).toStdString());
    }

    KnobIPtr divisionsKnob = globalTimeBlurNode->getKnobByName(kTimeBlurParamDivisions);
    KnobIPtr shutterKnob = globalTimeBlurNode->getKnobByName(kTimeBlurParamShutter);
    KnobIPtr shutterTypeKnob = globalTimeBlurNode->getKnobByName(kTimeBlurParamShutterOffset);
    KnobIPtr shutterCustomOffsetKnob = globalTimeBlurNode->getKnobByName(kTimeBlurParamCustomOffset);
    KnobBoolPtr disabledKnob = globalTimeBlurNode->getEffectInstance()->getDisabledKnob();

    assert(disabledKnob && divisionsKnob && shutterKnob && shutterTypeKnob && shutterCustomOffsetKnob);
    if (rotoPaintEffect->getMotionBlurTypeKnob()) {
        // The global time blur is disabled if the motion blur is set to per-shape
        std::string expression = "thisGroup.motionBlurMode.get() != 2";
        try {
            disabledKnob->setExpression(DimSpec(0), ViewSetSpec(0), expression, false, true);
        } catch (...) {
            assert(false);
        }
    }
    divisionsKnob->linkTo(globalMotionBlurKnob.lock());
    shutterKnob->linkTo(globalShutterKnob.lock());
    shutterTypeKnob->linkTo(globalShutterTypeKnob.lock());
    shutterCustomOffsetKnob->linkTo(globalCustomOffsetKnob.lock());
    return globalTimeBlurNode;
}

NodePtr
RotoPaintPrivate::getOrCreateGlobalMergeNode(int blendingOperator, int *availableInputIndex)
{
    {
        QMutexLocker k(&globalMergeNodesMutex);
        for (NodesList::iterator it = globalMergeNodes.begin(); it != globalMergeNodes.end(); ++it) {
            const std::vector<NodeWPtr > &inputs = (*it)->getInputs();

            // Merge node goes like this: B, A, Mask, A2, A3, A4 ...
            assert( inputs.size() >= 3 && (*it)->getEffectInstance()->isInputMask(2) );
            if ( !inputs[1].lock() ) {
                *availableInputIndex = 1;
                if (blendingOperator != -1) {
                    setOperationKnob(*it, blendingOperator);
                }
                return *it;
            }

            //Leave the B empty to connect the next merge node
            for (std::size_t i = 3; i < inputs.size(); ++i) {
                if ( !inputs[i].lock() ) {
                    *availableInputIndex = (int)i;
                    if (blendingOperator != -1) {
                        setOperationKnob(*it, blendingOperator);
                    }
                    return *it;
                }
            }
        }
    }


    NodePtr node = publicInterface->getNode();
    RotoPaintPtr rotoPaintEffect = toRotoPaint(node->getEffectInstance());


    //We must create a new merge node
    QString fixedNamePrefix = QString::fromUtf8( node->getScriptName_mt_safe().c_str() );

    fixedNamePrefix.append( QLatin1Char('_') );
    fixedNamePrefix.append( QString::fromUtf8("globalMerge") );
    fixedNamePrefix.append( QLatin1Char('_') );


    CreateNodeArgsPtr args(CreateNodeArgs::create( PLUGINID_OFX_MERGE,  rotoPaintEffect ));
#ifndef ROTO_PAINT_NODE_GRAPH_VISIBLE
    args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
#endif
    args->setProperty<bool>(kCreateNodeArgsPropVolatile, true);
    args->setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, fixedNamePrefix.toStdString());

    NodePtr mergeNode = node->getApp()->createNode(args);
    if (!mergeNode) {
        return mergeNode;
    }

    {
        // Link OpenGL enabled knob to the one on the Rotopaint so the user can control if GPU rendering is used in the roto internal node graph
        KnobChoicePtr glRenderKnob = mergeNode->getEffectInstance()->getOrCreateOpenGLEnabledKnob();
        if (glRenderKnob) {
            KnobChoicePtr rotoPaintGLRenderKnob = publicInterface->getOrCreateOpenGLEnabledKnob();
            assert(rotoPaintGLRenderKnob);
            bool ok = glRenderKnob->linkTo(rotoPaintGLRenderKnob);
            assert(ok);
            (void)ok;
        }
    }
    *availableInputIndex = 1;
    if (blendingOperator != -1) {
        setOperationKnob(mergeNode, blendingOperator);
    }

    {
        // Link the RGBA enabled checkbox of the Rotopaint to the merge output RGBA
        KnobBoolPtr rotoPaintRGBA[4];
        KnobBoolPtr mergeRGBA[4];
        rotoPaintEffect->getEnabledChannelKnobs(&rotoPaintRGBA[0], &rotoPaintRGBA[1], &rotoPaintRGBA[2], &rotoPaintRGBA[3]);
        mergeRGBA[0] = toKnobBool(mergeNode->getKnobByName(kMergeParamOutputChannelsR));
        mergeRGBA[1] = toKnobBool(mergeNode->getKnobByName(kMergeParamOutputChannelsG));
        mergeRGBA[2] = toKnobBool(mergeNode->getKnobByName(kMergeParamOutputChannelsB));
        mergeRGBA[3] = toKnobBool(mergeNode->getKnobByName(kMergeParamOutputChannelsA));
        for (int i = 0; i < 4; ++i) {
            bool ok = mergeRGBA[i]->linkTo(rotoPaintRGBA[i]);
            assert(ok);
            (void)ok;
        }

        // Link mix
        KnobIPtr rotoPaintMix;
        if (nodeType != RotoPaint::eRotoPaintTypeComp) {
            rotoPaintMix = rotoPaintEffect->getOrCreateHostMixKnob(rotoPaintEffect->getOrCreateMainPage());
            KnobIPtr mergeMix = mergeNode->getKnobByName(kMergeOFXParamMix);
            bool ok = mergeMix->linkTo(rotoPaintMix);
            assert(ok);
            (void)ok;
        }


    }


    QMutexLocker k(&globalMergeNodesMutex);
    globalMergeNodes.push_back(mergeNode);
    
    return mergeNode;
} // getOrCreateGlobalMergeNode

void
RotoPaintPrivate::connectRotoPaintBottomTreeToItems(bool /*canConcatenate*/,
                                                    const RotoPaintPtr& rotoPaintEffect,
                                                    const NodePtr& noOpNode,
                                                    const NodePtr& premultNode,
                                                    const NodePtr& globalTimeBlurNode,
                                                    const NodePtr& treeOutputNode,
                                                    const NodePtr& mergeNode)
{
    // If there's a noOp node (Roto/RotoPaint but not LayeredComp) connect the Output node of the tree to the NoOp node
    // otherwise connect it directly to the Merge node (LayeredComp)
    NodePtr treeOutputNodeInput = noOpNode ? noOpNode : mergeNode;
    treeOutputNode->swapInput(treeOutputNodeInput, 0);

    // If there's a NoOp node, connect it to the premult node
    if (noOpNode && premultNode) {
        noOpNode->swapInput(premultNode, 0);
    }

    // If there's a Premult node, connect it to the global TimeBlur node (if global motion-blur is enabled)
    // otherwise conneet it directly to the merge Node
    if (premultNode) {
        if (globalTimeBlurNode) {
            premultNode->swapInput(globalTimeBlurNode, 0);
            globalTimeBlurNode->swapInput(mergeNode, 0);
        } else {
            premultNode->swapInput(mergeNode, 0);
        }
    }

    // Connect the mask of the merge to the Mask input
    if (nodeType == RotoPaint::eRotoPaintTypeRoto ||
        nodeType == RotoPaint::eRotoPaintTypeRotoPaint) {
        mergeNode->swapInput(rotoPaintEffect->getInternalInputNode(ROTOPAINT_MASK_INPUT_INDEX), 2);
    }

}

void
RotoPaint::refreshRotoPaintTree()
{
    // Rebuild the internal tree of the RotoPaint node group from items and parameters.

    if (_imp->treeRefreshBlocked) {
        return;
    }
    TimeValue time = getCurrentTime_TLS();
    ViewIdx view = getCurrentView_TLS();

    // Get the items by render order. In the GUI they appear from bottom to top.
    std::list<RotoDrawableItemPtr > items = _imp->knobsTable->getRotoItemsByRenderOrder(time, view, false);

    // Check if the tree can be concatenated into a single merge node
    int blendingOperator = -1;
    bool canConcatenate = _imp->isRotoPaintTreeConcatenatableInternal(items, &blendingOperator);
    NodePtr globalMerge;
    int globalMergeIndex = -1;


    // Get the first global merge node.
    globalMerge = _imp->getOrCreateGlobalMergeNode(blendingOperator, &globalMergeIndex);


    {
        NodesList mergeNodes;
        {
            QMutexLocker k(&_imp->globalMergeNodesMutex);
            mergeNodes = _imp->globalMergeNodes;
        }

        // Ensure that all global merge nodes are disconnected so that items don't have output references
        // to the global merge nodes.
        for (NodesList::iterator it = mergeNodes.begin(); it != mergeNodes.end(); ++it) {
            int maxInputs = (*it)->getMaxInputCount();
            for (int i = 0; i < maxInputs; ++i) {
                (*it)->disconnectInput(i);
            }
        }
    }

    RotoPaintPtr rotoPaintEffect = toRotoPaint(getNode()->getEffectInstance());
    assert(rotoPaintEffect);

    // If concatenation enabled, connect the B input of the global Merge to the RotoPaint
    // background input node.
    if (canConcatenate) {
        NodePtr rotopaintNodeInput = rotoPaintEffect->getInternalInputNode(0);
        if (rotopaintNodeInput) {
            globalMerge->swapInput(rotopaintNodeInput, 0);
        }
    }

    // Refresh each item separately
    // Also place items in the node-graph
    Point nodePosition = {0.,0.};
    Point mergeNodeBeginPos = {0, 300};
    for (std::list<RotoDrawableItemPtr >::const_iterator it = items.begin(); it != items.end(); ++it) {
        (*it)->refreshNodesConnections();
        (*it)->refreshNodesPositions(nodePosition.x, nodePosition.y);

        // Place each item tree on the right
        nodePosition.x += 200;

        if (canConcatenate) {

            // If we concatenate the tree, connect the global merge Ax input to the effect

            NodePtr mergeInputA = (*it)->getMergeNode()->getInput(1);
            if (mergeInputA) {
                //qDebug() << "Connecting" << (*it)->getScriptName().c_str() << "to input" << globalMergeIndex <<
                //"(" << globalMerge->getInputLabel(globalMergeIndex).c_str() << ")" << "of" << globalMerge->getScriptName().c_str();
                globalMerge->swapInput(mergeInputA, globalMergeIndex);

                // If the global merge node has all its A inputs connected, create a new one, otherwise get the next A input.
                NodePtr nextMerge = _imp->getOrCreateGlobalMergeNode(blendingOperator, &globalMergeIndex);
                if (nextMerge != globalMerge) {

                    // Place the global merge below at the average of all nodes used
                    mergeNodeBeginPos.x = (nodePosition.x + mergeNodeBeginPos.x) / 2.;
                    globalMerge->setPosition(mergeNodeBeginPos.x, mergeNodeBeginPos.y);

                    mergeNodeBeginPos.y -= 200;

                    // If we made a new merge node, connect the B input of the new merge to the previous global merge.
                    assert( !nextMerge->getInput(0) );
                    nextMerge->connectInput(globalMerge, 0);
                    globalMerge = nextMerge;
                }
            }
        }
    }

    // Refresh the last global merge position
    {
        mergeNodeBeginPos.x = (nodePosition.x + mergeNodeBeginPos.x) / 2.;
        globalMerge->setPosition(mergeNodeBeginPos.x, mergeNodeBeginPos.y);
    }

    // At this point all items have their tree OK, now just connect the bottom of the tree
    // to the first item merge node.

    // Default to noop node as bottom of the tree (if any)
    KnobChoicePtr mbTypeKnob = _imp->motionBlurTypeKnob.lock();
    RotoMotionBlurModeEnum mbType = eRotoMotionBlurModeNone;
    if (mbTypeKnob) {
        mbType = (RotoMotionBlurModeEnum)mbTypeKnob->getValue();
    }
    NodePtr timeBlurNode = _imp->getOrCreateGlobalTimeBlurNode();
    if (mbType != eRotoMotionBlurModeGlobal) {
        timeBlurNode->disconnectInput(0);
    }
    mergeNodeBeginPos.y += 150;
    timeBlurNode->setPosition(mergeNodeBeginPos.x, mergeNodeBeginPos.y);

    NodePtr premultNode = rotoPaintEffect->getPremultNode();
    if (premultNode) {
        mergeNodeBeginPos.y += 150;
        premultNode->setPosition(mergeNodeBeginPos.x, mergeNodeBeginPos.y);
    }
    NodePtr noOpNode = rotoPaintEffect->getMetadataFixerNode();
    if (noOpNode) {
        mergeNodeBeginPos.y += 150;
        noOpNode->setPosition(mergeNodeBeginPos.x, mergeNodeBeginPos.y);
    }
    NodePtr treeOutputNode = rotoPaintEffect->getOutputNode();
    if (!treeOutputNode) {
        // should not happen
        return;
    }

    mergeNodeBeginPos.y += 150;
    treeOutputNode->setPosition(mergeNodeBeginPos.x - 100, mergeNodeBeginPos.y);

    // Also refresh the position of inputs nodes
    {
        std::vector<NodePtr> inputs(_imp->inputNodes.size());
        double totalInputsWidth = 0;
        for (std::size_t i = 0; i < _imp->inputNodes.size(); ++i) {
            NodePtr inputNode = _imp->inputNodes[i].lock();
            inputs[i] = inputNode;
            if (!inputNode) {
                continue;
            }
            double w,h;
            inputNode->getSize(&w, &h);
            totalInputsWidth += w;
            if (i < _imp->inputNodes.size() - 1) {
                totalInputsWidth += 200;
            }
        }
        Point inputPos = {nodePosition.x / 2. - totalInputsWidth / 2., nodePosition.y - 500};
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            NodePtr inputNode = inputs[i];
            if (!inputNode) {
                continue;
            }
            inputNode->setPosition(inputPos.x, inputPos.y);
            inputPos.x += 200;
        }
    }


    if (canConcatenate) {
        // Connect the bottom of the tree to the last global merge node.
        _imp->connectRotoPaintBottomTreeToItems(canConcatenate, rotoPaintEffect, noOpNode, premultNode,  timeBlurNode, treeOutputNode, _imp->globalMergeNodes.back());
    } else {

        if (!items.empty()) {
            // Connect the bottom of the tree to the last item merge node.
            _imp->connectRotoPaintBottomTreeToItems(canConcatenate, rotoPaintEffect, noOpNode, premultNode, timeBlurNode, treeOutputNode, items.back()->getMergeNode());
        } else {
            // Connect output to Input, the RotoPaint is pass-through
            treeOutputNode->swapInput(rotoPaintEffect->getInternalInputNode(0), 0);
        }
    }

    if (premultNode) {
        // Make sure the premult node has its RGB checkbox checked
        premultNode->getEffectInstance()->beginChanges();
        KnobBoolPtr process[3];
        process[0] = toKnobBool(premultNode->getKnobByName(kNatronOfxParamProcessR));
        process[1] = toKnobBool(premultNode->getKnobByName(kNatronOfxParamProcessG));
        process[2] = toKnobBool(premultNode->getKnobByName(kNatronOfxParamProcessB));
        for (int i = 0; i < 3; ++i) {
            assert(process[i]);
            process[i]->setValue(true);
        }
        premultNode->getEffectInstance()->endChanges();
        
    }
    
    
} // RotoPaint::refreshRotoPaintTree



///Must be done here because at the time of the constructor, the shared_ptr doesn't exist yet but
///addLayer() needs it to get a shared ptr to this
void
RotoPaintPrivate::createBaseLayer()
{
    ////Add the base layer
    RotoLayerPtr base = publicInterface->addLayerInternal();
    knobsTable->removeFromSelection(base, eTableChangeReasonInternal);
}

RotoLayerPtr
RotoPaint::getOrCreateBaseLayer()
{
    if (_imp->knobsTable->getNumTopLevelItems() == 0) {
        return addLayer();
    } else {
        return toRotoLayer(_imp->knobsTable->getTopLevelItem(0));
    }
}

RotoLayerPtr
RotoPaint::addLayerInternal()
{

    // If there's a selected layer, add it to it
    RotoLayerPtr parentLayer;
    parentLayer = toRotoLayer(_imp->knobsTable->findDeepestSelectedItemContainer());

    // Otherwise use the base layer
    if (!parentLayer) {
        parentLayer = toRotoLayer(_imp->knobsTable->getTopLevelItem(0));
    }

    RotoLayerPtr item(new RotoLayer(_imp->knobsTable));
    _imp->knobsTable->addItem(item, parentLayer, eTableChangeReasonInternal);
    _imp->knobsTable->beginEditSelection();
    _imp->knobsTable->clearSelection(eTableChangeReasonInternal);
    _imp->knobsTable->addToSelection(item, eTableChangeReasonInternal);
    _imp->knobsTable->endEditSelection(eTableChangeReasonInternal);

    return item;
} // ddLayerInternal

RotoLayerPtr
RotoPaint::addLayer()
{
    return addLayerInternal();
}

RotoLayerPtr
RotoPaint::getLayerForNewItem() 
{
    RotoLayerPtr parentLayer;
    parentLayer = toRotoLayer(_imp->knobsTable->findDeepestSelectedItemContainer());

    // Otherwise use the base layer
    if (!parentLayer) {
        parentLayer = toRotoLayer(_imp->knobsTable->getTopLevelItem(0));
    }

    // Otherwise create the base layer
    if (!parentLayer) {
        parentLayer = addLayer();
    }
    return parentLayer;
}

CompNodeItemPtr
RotoPaint::makeCompNodeItem()
{
    CompNodeItemPtr item(new CompNodeItem(_imp->knobsTable));
    _imp->knobsTable->insertItem(0, item, RotoLayerPtr(), eTableChangeReasonInternal);

    _imp->knobsTable->beginEditSelection();
    _imp->knobsTable->clearSelection(eTableChangeReasonInternal);
    _imp->knobsTable->addToSelection(item, eTableChangeReasonInternal);
    _imp->knobsTable->endEditSelection(eTableChangeReasonInternal);
    return item;
}


BezierPtr
RotoPaint::makeBezier(double x,
                        double y,
                        const std::string & baseName,
                        TimeValue time,
                        bool isOpenBezier)
{

    RotoLayerPtr parentLayer = getLayerForNewItem();
    BezierPtr curve( new Bezier(_imp->knobsTable, baseName,  isOpenBezier) );
    _imp->knobsTable->insertItem(0, curve, parentLayer, eTableChangeReasonInternal);

    _imp->knobsTable->beginEditSelection();
    _imp->knobsTable->clearSelection(eTableChangeReasonInternal);
    _imp->knobsTable->addToSelection(curve, eTableChangeReasonInternal);
    _imp->knobsTable->endEditSelection(eTableChangeReasonInternal);

    if ( curve->isAutoKeyingEnabled() ) {
        curve->setKeyFrame(time, ViewIdx(0), 0);
    }
    curve->addControlPoint(x, y, time, ViewIdx(0));

    return curve;
} // makeBezier

RotoStrokeItemPtr
RotoPaint::makeStroke(RotoStrokeType type,
                        bool clearSel)
{
    RotoLayerPtr parentLayer = getLayerForNewItem();
    RotoStrokeItemPtr curve( new RotoStrokeItem(type, _imp->knobsTable) );
    _imp->knobsTable->insertItem(0, curve, parentLayer, eTableChangeReasonInternal);

    if (clearSel) {
        _imp->knobsTable->beginEditSelection();
        _imp->knobsTable->clearSelection(eTableChangeReasonInternal);
        _imp->knobsTable->addToSelection(curve, eTableChangeReasonInternal);
        _imp->knobsTable->endEditSelection(eTableChangeReasonInternal);
    }

    return curve;
}

BezierPtr
RotoPaint::makeEllipse(double x,
                         double y,
                         double diameter,
                         bool fromCenter,
                         TimeValue time)
{
    double half = diameter / 2.;
    BezierPtr curve = makeBezier(x, fromCenter ? y - half : y, tr(kRotoEllipseBaseName).toStdString(), time, false);

    if (fromCenter) {
        curve->addControlPoint(x + half, y, time, ViewIdx(0));
        curve->addControlPoint(x, y + half, time, ViewIdx(0));
        curve->addControlPoint(x - half, y, time, ViewIdx(0));
    } else {
        curve->addControlPoint(x + diameter, y - diameter, time, ViewIdx(0));
        curve->addControlPoint(x, y - diameter, time, ViewIdx(0));
        curve->addControlPoint(x - diameter, y - diameter, time, ViewIdx(0));
    }

    BezierCPPtr top = curve->getControlPointAtIndex(0, ViewIdx(0));
    BezierCPPtr right = curve->getControlPointAtIndex(1, ViewIdx(0));
    BezierCPPtr bottom = curve->getControlPointAtIndex(2, ViewIdx(0));
    BezierCPPtr left = curve->getControlPointAtIndex(3, ViewIdx(0));
    double topX, topY, rightX, rightY, btmX, btmY, leftX, leftY;
    top->getPositionAtTime(time, &topX, &topY);
    right->getPositionAtTime(time,  &rightX, &rightY);
    bottom->getPositionAtTime(time,  &btmX, &btmY);
    left->getPositionAtTime(time,  &leftX, &leftY);

    curve->setLeftBezierPoint(0, time, ViewIdx(0), (leftX + topX) / 2., topY);
    curve->setRightBezierPoint(0, time, ViewIdx(0),(rightX + topX) / 2., topY);

    curve->setLeftBezierPoint(1, time, ViewIdx(0), rightX, (rightY + topY) / 2.);
    curve->setRightBezierPoint(1, time, ViewIdx(0),rightX, (rightY + btmY) / 2.);

    curve->setLeftBezierPoint(2, time,  ViewIdx(0),(rightX + btmX) / 2., btmY);
    curve->setRightBezierPoint(2, time, ViewIdx(0),(leftX + btmX) / 2., btmY);

    curve->setLeftBezierPoint(3, time, ViewIdx(0),  leftX, (btmY + leftY) / 2.);
    curve->setRightBezierPoint(3, time, ViewIdx(0),leftX, (topY + leftY) / 2.);
    curve->setCurveFinished(true, ViewIdx(0));

    return curve;
}

BezierPtr
RotoPaint::makeSquare(double x,
                        double y,
                        double initialSize,
                        TimeValue time)
{
    BezierPtr curve = makeBezier(x, y, kRotoRectangleBaseName, time, false);

    curve->addControlPoint(x + initialSize, y, time, ViewIdx(0));
    curve->addControlPoint(x + initialSize, y - initialSize, time, ViewIdx(0));
    curve->addControlPoint(x, y - initialSize, time, ViewIdx(0));
    curve->setCurveFinished(true, ViewIdx(0));
    
    return curve;
}

KnobChoicePtr
RotoPaint::getMergeAInputChoiceKnob() const
{
    return _imp->mergeInputAChoiceKnob.lock();
}

KnobChoicePtr
RotoPaint::getMotionBlurTypeKnob() const
{
    return _imp->motionBlurTypeKnob.lock();
}

KnobDoublePtr
RotoPaint::getMixKnob() const
{
    return _imp->mixKnob.lock();
}

void
RotoPaint::refreshSourceKnobs(const RotoDrawableItemPtr& item)
{
    std::vector<ChoiceOption> inputAChoices, maskChoices;
    getMergeChoices(&inputAChoices, &maskChoices);
    {
        KnobChoicePtr itemSourceKnob = item->getMergeInputAChoiceKnob();
        if (itemSourceKnob) {
            itemSourceKnob->populateChoices(inputAChoices);
        }
    }
    {
        KnobChoicePtr maskSourceKnob = item->getMergeMaskChoiceKnob();
        if (maskSourceKnob) {
            maskSourceKnob->populateChoices(maskChoices);
        }
    }
}

void
RotoPaint::getMergeChoices(std::vector<ChoiceOption>* inputAChoices, std::vector<ChoiceOption>* maskChoices) const
{
    ChoiceOption noneChoice("None", tr("None").toStdString(), "");
    maskChoices->push_back(noneChoice);
    if (_imp->nodeType != RotoPaint::eRotoPaintTypeComp) {
        inputAChoices->push_back(ChoiceOption("Foreground", tr("Foreground").toStdString(), ""));
    } else {
        inputAChoices->push_back(noneChoice);
    }
    for (int i = 1; i < LAYERED_COMP_MAX_INPUTS_COUNT; ++i) {
        EffectInstancePtr input = getInput(i);
        if (!input) {
            continue;
        }
        QObject::connect(input->getNode().get(), SIGNAL(labelChanged(QString,QString)), this, SLOT(onSourceNodeLabelChanged(QString,QString)), Qt::UniqueConnection);
        const std::string& inputLabel = input->getNode()->getLabel();
        ChoiceOption opt(QString::number(i).toStdString(), inputLabel, "");
        bool isMask = i >= LAYERED_COMP_FIRST_MASK_INPUT_INDEX;
        if (!isMask) {
            inputAChoices->push_back(opt);
        } else {
            maskChoices->push_back(opt);
        }
    }
}



void
RotoPaintPrivate::refreshSourceKnobs()
{

    std::vector<ChoiceOption> inputAChoices, maskChoices;
    publicInterface->getMergeChoices(&inputAChoices, &maskChoices);


    KnobChoicePtr inputAKnob = mergeInputAChoiceKnob.lock();
    if (inputAKnob) {
        inputAKnob->populateChoices(inputAChoices);
    }


    // Refresh all items menus aswell
    std::list< RotoDrawableItemPtr > drawables = knobsTable->getRotoItemsByRenderOrder(publicInterface->getTimelineCurrentTime(), ViewIdx(0), false);
    for (std::list< RotoDrawableItemPtr > ::const_iterator it = drawables.begin(); it != drawables.end(); ++it) {
        {
            KnobChoicePtr itemSourceKnob = (*it)->getMergeInputAChoiceKnob();
            if (itemSourceKnob) {
                itemSourceKnob->populateChoices(inputAChoices);
            }
        }
        {
            KnobChoicePtr maskSourceKnob = (*it)->getMergeMaskChoiceKnob();
            if (maskSourceKnob) {
                maskSourceKnob->populateChoices(maskChoices);
            }
        }
    }
} // refreshSourceKnobs

void
RotoPaint::onSourceNodeLabelChanged(const QString& /*oldLabel*/, const QString& /*newLabel*/)
{

    _imp->refreshSourceKnobs();
}

void
RotoPaint::addSoloItem(const RotoDrawableItemPtr& item)
{
    QMutexLocker k(&_imp->soloItemsMutex);
    _imp->soloItems.insert(item);
}

bool
RotoPaint::isAmongstSoloItems(const RotoDrawableItemPtr& item) const
{
    QMutexLocker k(&_imp->soloItemsMutex);
    if (_imp->soloItems.empty()) {
        return true;
    }
    std::set<RotoDrawableItemWPtr>::const_iterator found = _imp->soloItems.find(item);
    return found != _imp->soloItems.end();
}

void
RotoPaint::removeSoloItem(const RotoDrawableItemPtr& item)
{
    QMutexLocker k(&_imp->soloItemsMutex);
    std::set<RotoDrawableItemWPtr>::iterator found = _imp->soloItems.find(item);
    if (found == _imp->soloItems.end()) {
        return;
    }
    _imp->soloItems.erase(found);
}

NATRON_NAMESPACE_EXIT;
NATRON_NAMESPACE_USING;
#include "moc_RotoPaint.cpp"


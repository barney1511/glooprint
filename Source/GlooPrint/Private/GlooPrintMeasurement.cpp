#include "GlooPrintMeasurement.h"
#include "GlooPrintMeasurementCache.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/ArrangedChildren.h"
#include "NodeFactory.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "Types/SlateAttributeMetaData.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"

namespace GlooPrint
{
namespace
{
constexpr int32 MaxWidgetsPerNode = 16384;

bool IsFinite(const FVector2f& Value)
{
    return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y);
}

void Include(FMeasuredRect& Bounds, const FVector2f& Min, const FVector2f& Max)
{
    Bounds.Min.X = FMath::Min(Bounds.Min.X, Min.X);
    Bounds.Min.Y = FMath::Min(Bounds.Min.Y, Min.Y);
    Bounds.Max.X = FMath::Max(Bounds.Max.X, Max.X);
    Bounds.Max.Y = FMath::Max(Bounds.Max.Y, Max.Y);
}

bool RefreshTextControls(const TSharedRef<SGraphNode>& Node, float LayoutScale)
{
    TArray<TSharedRef<SWidget>> Pending { Node };
    int32 Visited = 0;
    while (!Pending.IsEmpty())
    {
        const TSharedRef<SWidget> Widget = Pending.Pop(EAllowShrinking::No);
        if (++Visited > MaxWidgetsPerNode)
        {
            return false;
        }
        if (Widget->GetType() == TEXT("SEditableText"))
        {
            Widget->Tick(
                FGeometry::MakeRoot(Widget->GetDesiredSize(), FSlateLayoutTransform(LayoutScale)),
                FSlateApplication::Get().GetCurrentTime(), 0.0f);
        }
        else if (Widget->GetType() == TEXT("SMultiLineEditableText"))
        {
            StaticCastSharedRef<SMultiLineEditableText>(Widget)->Refresh();
        }
        FChildren* Children = Widget->GetChildren();
        if (Visited + Pending.Num() + Children->Num() > MaxWidgetsPerNode)
        {
            return false;
        }
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            Pending.Add(Children->GetChildAt(Index));
        }
    }
    return true;
}

bool IsHiddenPin(const UEdGraphPin& Pin, SGraphEditor::EPinVisibility Visibility)
{
    if (!Pin.LinkedTo.IsEmpty()) { return false; }
    if (Pin.bHidden || (Pin.bAdvancedView && Pin.GetOwningNode()->AdvancedPinDisplay == ENodeAdvancedPins::Hidden)) { return true; }
    if (Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) { return false; }
    if (Visibility == SGraphEditor::Pin_HideNoConnection) { return true; }
    if (Visibility == SGraphEditor::Pin_HideNoConnectionNoDefault)
    {
        const bool bSelf = Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Object && Pin.PinName == UEdGraphSchema_K2::PN_Self;
        return Pin.Direction != EGPD_Input || (Pin.DefaultValue.IsEmpty() && !Pin.DefaultObject && !bSelf);
    }
    return false;
}

struct FRestoreNodeWidgetReference
{
    TWeakObjectPtr<UEdGraphNode> Node;
    TWeakPtr<SGraphNode> Previous;
    explicit FRestoreNodeWidgetReference(UEdGraphNode& InNode) : Node(&InNode), Previous(InNode.DEPRECATED_NodeWidget) {}
    ~FRestoreNodeWidgetReference()
    {
        if (UEdGraphNode* Live = Node.Get()) { Live->DEPRECATED_NodeWidget = Previous; }
    }
};

bool MeasureNode(UEdGraphNode& Node, float LayoutScale, FMeasuredNode& Out, FString& Reason,
    const TSharedPtr<SGraphPanel>& MeasurementPanel, SGraphEditor::EPinVisibility Visibility, bool& bNeedsRetry,
    TOptional<int32> ProposedCommentWidth = {})
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_MeasureNode);
    Out.Pins.Reserve(Node.Pins.Num());
    TMap<const UEdGraphPin*, int32> PinIndices;
    PinIndices.Reserve(Node.Pins.Num());
    TSet<FGuid> PinIds;
    for (const UEdGraphPin* Pin : Node.Pins)
    {
        if (!Pin || Pin->bWasTrashed || Pin->GetOwningNodeUnchecked() != &Node
            || !Pin->PinId.IsValid() || PinIds.Contains(Pin->PinId)
            || (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output))
        {
            Reason = TEXT("The node has missing, reconstructed, or ambiguous pin identities.");
            return false;
        }
        PinIds.Add(Pin->PinId);
        PinIndices.Add(Pin, Out.Pins.Num());
        Out.Pins.Add({ Pin->PinId, {} });
    }


    FRestoreNodeWidgetReference RestoreWidgetReference(Node);
    const TSharedPtr<SGraphNode> Widget = FNodeFactory::CreateNodeWidget(&Node);
    if (!Widget || Widget->RequiresSecondPassLayout())
    {
        Reason = TEXT("The node requires an unavailable widget or dependent layout.");
        return false;
    }
    if (MeasurementPanel) { Widget->SetOwner(MeasurementPanel.ToSharedRef()); }
    if (ProposedCommentWidth.IsSet())
    {
        if (Widget->GetType() != TEXT("SGraphNodeComment"))
        {
            Reason = TEXT("The custom comment widget cannot measure a proposed title width."); return false;
        }
        TArray<TSharedRef<SWidget>> Pending{Widget.ToSharedRef()};
        TSharedPtr<SInlineEditableTextBlock> Title;
        for (int32 I = 0; I < Pending.Num(); ++I)
        {
            const auto Current = Pending[I];
            if (Current->GetType() == TEXT("SInlineEditableTextBlock"))
            {
                if (Title) { Reason = TEXT("The comment widget has an ambiguous title."); return false; }
                Title = StaticCastSharedRef<SInlineEditableTextBlock>(Current);
            }
            FChildren* Children = Current->GetChildren();
            if (Pending.Num() + Children->Num() > MaxWidgetsPerNode)
            {
                Reason = TEXT("The comment widget exceeds the measurement limit."); return false;
            }
            for (int32 C = 0; C < Children->Num(); ++C) { Pending.Add(Children->GetChildAt(C)); }
        }
        if (!Title) { Reason = TEXT("The native comment title widget is unavailable."); return false; }
        Title->SetWrapTextAt(float(ProposedCommentWidth.GetValue()) - 32.f);
    }
    FVector2f Size = FVector2f::ZeroVector;
    bool bSizeSettled = false;
    for (int32 Pass = 0; Pass < 4; ++Pass)
    {
        Widget->MarkPrepassAsDirty();
        Widget->SlatePrepass(LayoutScale);
        FSlateAttributeMetaData::UpdateAllAttributes(*Widget,
            FSlateAttributeMetaData::EInvalidationPermission::AllowInvalidationIfConstructed);
        const FVector2f NextSize(Widget->GetDesiredSize());
        if (Pass == 0 && !RefreshTextControls(Widget.ToSharedRef(), LayoutScale))
        {
            Reason = TEXT("The native widget hierarchy exceeds the measurement limit.");
            return false;
        }
        bSizeSettled = Pass > 0 && NextSize == Size;
        Size = NextSize;
        if (bSizeSettled)
        {
            break;
        }
    }
    if (!bSizeSettled || !IsFinite(Size) || Size.X <= 0.0f || Size.Y <= 0.0f)
    {
        bNeedsRetry = IsFinite(Size);
        Reason = TEXT("The native node widget has no reliable desired size.");
        return false;
    }

    Out.Id = Node.NodeGuid;
    Out.Position = FIntPoint(Node.NodePosX, Node.NodePosY);
    Out.BodySize = Size;
    Out.VisualBounds = { FVector2f::ZeroVector, Size };

    TArray<TSharedRef<SWidget>> PinWidgets;
    Widget->GetPins(PinWidgets);
    TMap<const SWidget*, int32> WidgetPins;
    WidgetPins.Reserve(PinWidgets.Num());
    for (const TSharedRef<SWidget>& PinWidget : PinWidgets)
    {
        const UEdGraphPin* Pin = StaticCastSharedRef<SGraphPin>(PinWidget)->GetPinObj();
        const int32* Index = PinIndices.Find(Pin);
        if (!Index)
        {
            Reason = TEXT("The native widget contains a pin outside this node snapshot.");
            return false;
        }
        WidgetPins.Add(&PinWidget.Get(), *Index);
    }

    TArray<FArrangedWidget> Pending;
    Pending.Emplace(Widget.ToSharedRef(), FGeometry::MakeRoot(Size, FSlateLayoutTransform()));
    int32 Visited = 0;
    while (!Pending.IsEmpty())
    {
        const FArrangedWidget Current = Pending.Pop(EAllowShrinking::No);
        if (++Visited > MaxWidgetsPerNode)
        {
            Reason = TEXT("The native widget hierarchy exceeds the measurement limit.");
            return false;
        }
        const FVector2f LocalSize(Current.Geometry.GetLocalSize());
        const FVector2f Min(Current.Geometry.LocalToAbsolute(FVector2f::ZeroVector));
        const FVector2f Max(Current.Geometry.LocalToAbsolute(LocalSize));
        if (!IsFinite(Min) || !IsFinite(Max) || Max.X < Min.X || Max.Y < Min.Y)
        {
            Reason = TEXT("The native widget supplied invalid arranged geometry.");
            return false;
        }
        Include(Out.VisualBounds, Min, Max);
        if (const int32* Index = WidgetPins.Find(&Current.Widget.Get()))
        {
            if (LocalSize.X <= 0.0f || LocalSize.Y <= 0.0f)
            {
                bNeedsRetry = true;
                Reason = TEXT("A visible pin has no reliable arranged geometry.");
                return false;
            }
            const UEdGraphPin* Pin = Node.Pins[*Index];
            const FVector2f Point(Pin->Direction == EGPD_Output ? LocalSize.X : 0.0f,
                LocalSize.Y * 0.5f);
            Out.Pins[*Index].AttachmentOffset = FVector2f(Current.Geometry.LocalToAbsolute(Point));
        }
        FArrangedChildren Children(EVisibility::Visible);
        Current.Widget->ArrangeChildren(Current.Geometry, Children, true);
        if (Visited + Pending.Num() + Children.Num() > MaxWidgetsPerNode)
        {
            Reason = TEXT("The native widget hierarchy exceeds the measurement limit.");
            return false;
        }
        for (int32 Index = Children.Num() - 1; Index >= 0; --Index)
        {
            Pending.Add(Children[Index]);
        }
    }

    for (int32 Index = 0; Index < Node.Pins.Num(); ++Index)
    {
        const UEdGraphPin& Pin = *Node.Pins[Index];
        if (!IsHiddenPin(Pin, Visibility) && !Out.Pins[Index].AttachmentOffset.IsSet())
        {
            bNeedsRetry = true;
            Reason = FString::Printf(TEXT("Required pin '%s' has no arranged attachment."), *Pin.PinName.ToString());
            return false;
        }
    }

    TArray<FOverlayBrushInfo> Brushes;
    Widget->GetOverlayBrushes(false, Size, Brushes);
    for (const FOverlayBrushInfo& Overlay : Brushes)
    {
        if (Overlay.Brush)
        {
            const FVector2f Offset(Overlay.OverlayOffset);
            const FVector2f Envelope(Overlay.AnimationEnvelope);
            const FVector2f BrushSize(Overlay.Brush->ImageSize);
            if (!IsFinite(Offset) || !IsFinite(Envelope) || !IsFinite(BrushSize))
            {
                Reason = TEXT("The node supplied invalid overlay bounds.");
                return false;
            }
            const FVector2f Extent(FMath::Abs(Envelope.X), FMath::Abs(Envelope.Y));
            Include(Out.VisualBounds, Offset - Extent, Offset + BrushSize + Extent);
        }
    }
    for (const FOverlayWidgetInfo& Overlay : Widget->GetOverlayWidgets(false, Size))
    {
        if (Overlay.Widget)
        {
            Overlay.Widget->SlatePrepass(LayoutScale);
            if (Overlay.Widget->GetVisibility().IsVisible())
            {
                const FVector2f Offset(Overlay.OverlayOffset);
                const FVector2f OverlaySize(Overlay.Widget->GetDesiredSize());
                if (!IsFinite(Offset) || !IsFinite(OverlaySize) || OverlaySize.X <= 0 || OverlaySize.Y <= 0)
                {
                    bNeedsRetry = IsFinite(Offset) && IsFinite(OverlaySize);
                    Reason = TEXT("The node supplied unavailable overlay geometry.");
                    return false;
                }
                Include(Out.VisualBounds, Offset, Offset + OverlaySize);
            }
        }
    }
    if (Node.IsA<UEdGraphNode_Comment>())
    {
        const FSlateRect Header = Widget->GetTitleRect();
        const FVector2f Origin(Out.Position);
        Out.CommentHeader = FMeasuredRect {
            FVector2f(Header.Left, Header.Top) - Origin,
            FVector2f(Header.Right, Header.Bottom) - Origin
        };
        const FMeasuredRect& Bounds = Out.CommentHeader.GetValue();
        if (!IsFinite(Bounds.Min) || !IsFinite(Bounds.Max)
            || Bounds.Max.X <= Bounds.Min.X || Bounds.Max.Y <= Bounds.Min.Y)
        {
            bNeedsRetry = IsFinite(Bounds.Min) && IsFinite(Bounds.Max);
            Reason = TEXT("The comment has no reliable header geometry.");
            return false;
        }
    }
    return true;
}
}

bool ValidateMeasurementGraph(UEdGraph* Graph, FString& OutReason)
{
    OutReason.Reset();
    if (!IsInGameThread() || !FSlateApplication::IsInitialized())
    {
        OutReason = TEXT("Measurement requires the editor thread and initialized Slate.");
        return false;
    }
    if (!IsValid(Graph) || !Graph->GetSchema() || Graph->GetSchema()->GetClass() != UEdGraphSchema_K2::StaticClass())
    {
        OutReason = TEXT("Measurement supports standard K2 Blueprint graphs only.");
        return false;
    }
    const UBlueprint* Blueprint = Graph->GetTypedOuter<UBlueprint>();
    if (!Blueprint || Blueprint->bBeingCompiled || Blueprint->bIsRegeneratingOnLoad)
    {
        OutReason = TEXT("A stable Blueprint owner is required; compilation or reconstruction is active.");
        return false;
    }

    TSet<FGuid> NodeIds;
    TSet<const UEdGraphPin*> Pins;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!IsValid(Node) || Node->GetGraph() != Graph || !Node->NodeGuid.IsValid() || NodeIds.Contains(Node->NodeGuid))
        {
            OutReason = TEXT("The graph has missing, reconstructed, or ambiguous node identities.");
            return false;
        }
        NodeIds.Add(Node->NodeGuid);
        TSet<FGuid> PinIds;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bWasTrashed || Pin->GetOwningNodeUnchecked() != Node || !Pin->PinId.IsValid() ||
                PinIds.Contains(Pin->PinId) || (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output))
            {
                OutReason = TEXT("A node contains invalid pin identities."); return false;
            }
            PinIds.Add(Pin->PinId); Pins.Add(Pin);
        }
    }
    for (const UEdGraphPin* Pin : Pins)
    {
        for (const UEdGraphPin* Link : Pin->LinkedTo)
        {
            if (!Pins.Contains(Link)) { OutReason = TEXT("A connection has a missing or external endpoint."); return false; }
        }
    }
    return true;
}

bool MeasureGraph(UEdGraph* Graph, float LayoutScale, FGraphMeasurement& OutMeasurement, FString& OutReason,
    const FMeasurementOptions& Options, bool* OutNeedsLayoutRetry)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_MeasureGraph);
    OutMeasurement.Nodes.Reset();
    if (OutNeedsLayoutRetry) { *OutNeedsLayoutRetry = false; }
    if (!ValidateMeasurementGraph(Graph, OutReason)) { return false; }
    if (!FMath::IsFinite(LayoutScale) || LayoutScale <= 0.0f)
    {
        OutReason = TEXT("A finite, positive display scale is required."); return false;
    }
    if (Options.PinVisibility != SGraphEditor::Pin_Show && Options.PinVisibility != SGraphEditor::Pin_HideNoConnection &&
        Options.PinVisibility != SGraphEditor::Pin_HideNoConnectionNoDefault)
    {
        OutReason = TEXT("The graph pin-visibility mode is invalid."); return false;
    }
    if (Options.Cache) { Options.Cache->Begin(Graph, LayoutScale, Options.PinVisibility); }
    FGraphMeasurement Candidate;
    Candidate.Nodes.Reserve(Graph->Nodes.Num());
    TSharedPtr<SGraphPanel> MeasurementPanel;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        TArray<uint8> State;
        if (Options.Cache)
        {
            State = CaptureMeasurementState(*Node);
            if (const FMeasuredNode* Cached = Options.Cache->Find(*Node, State))
            {
                Candidate.Nodes.Add(*Cached); continue;
            }
        }
        if (!MeasurementPanel && Options.PinVisibility != SGraphEditor::Pin_Show)
        {
            MeasurementPanel = SNew(SGraphPanel).GraphObj(Graph).IsEditable(true).InitialZoomToFit(false);
            MeasurementPanel->SetPinVisibility(Options.PinVisibility);
        }
        FMeasuredNode Measured;
        bool bNeedsRetry = false;
        if (!MeasureNode(*Node, LayoutScale, Measured, OutReason, MeasurementPanel, Options.PinVisibility, bNeedsRetry))
        {
            if (OutNeedsLayoutRetry) { *OutNeedsLayoutRetry = bNeedsRetry; }
            OutReason = FString::Printf(TEXT("%s: %s"), *Node->GetName(), *OutReason);
            return false;
        }
        if (Options.Cache) { Options.Cache->Store(*Node, MoveTemp(State), Measured); }
        Candidate.Nodes.Add(MoveTemp(Measured));
    }
    Candidate.Nodes.Sort([](const FMeasuredNode& A, const FMeasuredNode& B) { return A.Id < B.Id; });
    OutMeasurement = MoveTemp(Candidate);
    return true;
}

bool MeasureCommentHeader(UEdGraphNode_Comment* Comment, float LayoutScale, int32 ProposedWidth,
    FMeasuredRect& OutHeader, FString& OutReason, bool* OutNeedsLayoutRetry)
{
    OutHeader = {}; OutReason.Reset();
    if (OutNeedsLayoutRetry) { *OutNeedsLayoutRetry = false; }
    if (!IsInGameThread() || !IsValid(Comment) || !FSlateApplication::IsInitialized() ||
        !FMath::IsFinite(LayoutScale) || LayoutScale <= 0 || ProposedWidth <= 32 || ProposedWidth > 16777216)
    {
        OutReason = TEXT("Proposed comment measurement requires a live comment and a valid width/display scale."); return false;
    }
    FMeasuredNode Measured; bool bRetry = false;
    if (!MeasureNode(*Comment, LayoutScale, Measured, OutReason, {}, SGraphEditor::Pin_Show, bRetry, ProposedWidth))
    {
        if (OutNeedsLayoutRetry) { *OutNeedsLayoutRetry = bRetry; }
        return false;
    }
    if (!Measured.CommentHeader.IsSet()) { OutReason = TEXT("The proposed comment has no measured title."); return false; }
    OutHeader = Measured.CommentHeader.GetValue();
    return true;
}
}

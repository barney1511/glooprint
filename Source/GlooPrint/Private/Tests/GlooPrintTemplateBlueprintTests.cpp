#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Algo/Reverse.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/ObjectWriter.h"
#include "SGraphPanel.h"
#include "UObject/LinkerInstancingContext.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FTemplateBlueprintCheck final : public IAutomationLatentCommand, public FOutputDevice
{
public:
    explicit FTemplateBlueprintCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FTemplateBlueprintCheck() { Restore(); }
    virtual bool CanBeUsedOnAnyThread() const override { return true; }
    virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
    virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
    {
        if (!IsInGameThread() || !bAwaitingFormat || Category != FName(TEXT("LogGlooPrintEditor"))) { return; }
        static constexpr TCHAR Prefix[] = TEXT("Formatted graph: ");
        LastMessage = Message;
        if (FCString::Strncmp(Message, Prefix, UE_ARRAY_COUNT(Prefix) - 1) == 0)
        {
            Changed = FCString::Atoi(Message + UE_ARRAY_COUNT(Prefix) - 1); bAwaitingFormat = false;
            Editor->GetViewLocation(CompletedView, CompletedZoom);
        }
    }
    virtual bool Update() override
    {
        if (!bInitialized) { return Start(); }
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test.AddError(TEXT("Template Blueprint check exceeded its deadline. ") + LastMessage); return Finish();
        }
        if (++Frames < 12) { return false; }
        auto& Slate = FSlateApplication::Get();
        auto* Panel = Editor->GetGraphPanel();
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady() || bAwaitingFormat) { return false; }
        FString Reason;
        const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
        if (Phase == 0)
        {
            const auto Original = SerializeNodes(*Graph);
            if (!Test.TestTrue(TEXT("Authored construction script has a complete format plan"),
                PlanFormatGraph(Graph, Scale, {Entry->NodeGuid}, Plan, Reason))) { Test.AddError(Reason); return Finish(); }
            Test.TestTrue(TEXT("Planning preserves authored graph data"), Original == SerializeNodes(*Graph));
            Test.AddInfo(FString::Printf(TEXT("Installed spline mesh construction script: %d nodes, %d pins, %d links, %d spacing repairs."),
                Plan.Snapshot.Nodes.Num(), Plan.Snapshot.Pins.Num(), Plan.Snapshot.Edges.Num(), Plan.SpacingRepairs));
            RecordMetrics(TEXT("AuthoredRounded"), Plan.Snapshot, Cache->GetRoutes());
            FBox2f Bounds(ForceInit);
            for (int32 I = 0; I < Plan.Snapshot.Nodes.Num(); ++I)
            {
                const auto& N = Plan.Snapshot.Nodes[I];
                Bounds += FVector2f(N.Geometry.Position); Bounds += FVector2f(N.Geometry.Position) + N.Geometry.BodySize;
                const FVector2f ProposedSize = N.bComment ? FVector2f(Plan.Layout.Sizes[I]) : N.Geometry.BodySize;
                Bounds += FVector2f(Plan.Layout.Positions[I]); Bounds += FVector2f(Plan.Layout.Positions[I]) + ProposedSize;
            }
            const FRouteSet* RouteSets[] = {&Plan.Routes, &Cache->GetRoutes()};
            for (const auto* Routes : RouteSets)
            {
                for (const auto& Pair : Routes->Wires) { Bounds += Pair.Value.Bounds.Min; Bounds += Pair.Value.Bounds.Max; }
            }
            const FVector2f Available = Panel->GetCachedGeometry().GetLocalSize();
            const FVector2f Span = Bounds.Max - Bounds.Min + FVector2f(160);
            const float FitZoom = FMath::Min(1.f, FMath::Min(Available.X / Span.X, Available.Y / Span.Y));
            const auto& Levels = Panel->GetZoomLevels(); float CaptureZoom = 0;
            for (int32 I = 0; I < Levels->GetNumZoomLevels(); ++I)
            {
                const float Amount = Levels->GetZoomAmount(I);
                if (Amount <= FitZoom) { CaptureZoom = FMath::Max(CaptureZoom, Amount); }
            }
            if (!Test.TestTrue(TEXT("A supported native zoom fits both complete layouts"), CaptureZoom > 0)) { return Finish(); }
            CaptureBounds = Bounds;
            Editor->SetViewLocation(Bounds.Min - FVector2f(80), CaptureZoom);
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            Capture(TEXT("BeforeRounded")); Before = SerializeTransactionValues(*Graph); Properties = DescribeNodes(*Graph);
            TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
            for (auto* Other : Graphs) { if (Other != Graph) { OtherGraphs.Add(Other, SerializeTransactionValues(*Other)); } }
            FObjectWriter DefaultWriter(Blueprint->GeneratedClass->GetDefaultObject(), Defaults);
            Editor->GetViewLocation(View, Zoom); Anchor = FIntPoint(Entry->NodePosX, Entry->NodePosY);
            Queue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            BeginFormat(); Phase = 2; return false;
        }
        if (Phase == 2)
        {
            Test.TestTrue(TEXT("Actual F formats the authored layout"), Changed > 0);
            Test.TestEqual(TEXT("Template F creates one undo action"), GEditor->Trans->GetQueueLength(), Queue + 1);
            CheckContext(); CheckProperties();
            for (const auto& Pair : OtherGraphs) { Test.TestTrue(TEXT("Formatting leaves other Blueprint graphs unchanged"), Pair.Value == SerializeTransactionValues(*Pair.Key)); }
            TArray<uint8> CurrentDefaults; FObjectWriter DefaultWriter(Blueprint->GeneratedClass->GetDefaultObject(), CurrentDefaults);
            Test.TestTrue(TEXT("Formatting leaves Blueprint class defaults unchanged"), Defaults == CurrentDefaults);
            After = SerializeTransactionValues(*Graph);
            Test.TestTrue(TEXT("Authored Blueprint format undoes"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Undo restores every authored graph value"), Before == SerializeTransactionValues(*Graph));
            Test.TestTrue(TEXT("Authored Blueprint format redoes"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Redo restores every formatted graph value"), After == SerializeTransactionValues(*Graph));
            if (!Compile()) { return Finish(); }
            PrimeTooltips(); After = SerializeTransactionValues(*Graph);
            Phase = 3; Frames = 0; return false;
        }
        if (Phase == 3 || Phase == 5)
        {
            const bool bDiagonal = Phase == 5;
            FFormatPlan Cold;
            if (!Test.TestTrue(TEXT("Template cold native geometry computes"), PlanFormatGraph(Graph, Scale, {Entry->NodeGuid}, Cold, Reason)))
            {
                Test.AddError(Reason); return Finish();
            }
            Test.TestTrue(TEXT("Template layout is idempotent with cold caches"), Cold.Layout.Positions == Plan.Layout.Positions && Cold.Layout.Sizes == Plan.Layout.Sizes);
            CheckRoutes(Cold, Cache->GetRoutes());
            FLayoutGraph Shuffled = Cold.Snapshot; Algo::Reverse(Shuffled.Edges); FRouteSet ShuffledRoutes;
            if (Test.TestTrue(TEXT("Reordered authored links route"), ComputeLayoutRoutes(Shuffled, Cold.Layout, ShuffledRoutes, Reason,
                bDiagonal ? EGlooPrintWireStyle::Diagonal45 : EGlooPrintWireStyle::Rounded90))) { CheckRoutes(Cold, ShuffledRoutes); }
            RecordMetrics(bDiagonal ? TEXT("FormattedDiagonal") : TEXT("FormattedRounded"), Cold.Snapshot, Cache->GetRoutes());
            Capture(bDiagonal ? TEXT("AfterDiagonal") : TEXT("AfterRounded"));
            Queue = GEditor->Trans->GetQueueLength(); Package->SetDirtyFlag(false); After = SerializeTransactionValues(*Graph);
            BeginFormat(); Phase = bDiagonal ? 6 : 4; return false;
        }
        Test.TestEqual(TEXT("Repeated F completes as a true no-op"), Changed, 0);
        Test.TestEqual(TEXT("No-op creates no undo action"), GEditor->Trans->GetQueueLength(), Queue);
        Test.TestTrue(TEXT("No-op preserves every graph value"), After == SerializeTransactionValues(*Graph));
        Test.TestFalse(TEXT("No-op leaves the private package clean"), Package->IsDirty()); CheckContext();
        if (Phase == 4)
        {
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Diagonal45;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged(); Phase = 5; Frames = 0; return false;
        }
        TArray<uint8> CurrentSource, CurrentCopy;
        Test.TestTrue(TEXT("Installed template bytes remain unchanged"), FFileHelper::LoadFileToArray(CurrentSource, *SourcePath) && CurrentSource == SourceBytes);
        Test.TestTrue(TEXT("Private asset file remains an untouched authored baseline"), FFileHelper::LoadFileToArray(CurrentCopy, *CopyPath) && CurrentCopy == SourceBytes);
        Test.TestTrue(TEXT("Save real Blueprint quality metrics"), FFileHelper::SaveStringToFile(Metrics, *(Directory / TEXT("metrics.csv"))));
        return Finish();
    }
private:
    bool Start()
    {
        bInitialized = true; Deadline = FPlatformTime::Seconds() + 90;
        SourcePath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("../Templates/TP_AEC_ArchvisBP/Content/ArchvisProject/Blueprints/BP_Splinemesh.uasset"));
        if (!Test.TestTrue(TEXT("Installed architectural spline template is available"), FFileHelper::LoadFileToArray(SourceBytes, *SourcePath))) { return true; }
        const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("TemplateBlueprint") / Id) + TEXT("/");
        Mount = TEXT("/GlooPrintTemplate_") + Id + TEXT("/"); CopyPath = Directory / TEXT("BP_Splinemesh.uasset");
        IFileManager::Get().MakeDirectory(*Directory, true);
        if (!Test.TestTrue(TEXT("Copy the authored asset into a private fixture directory"), FFileHelper::SaveArrayToFile(SourceBytes, *CopyPath))) { return true; }
        FPackageName::RegisterMountPoint(Mount, Directory); bMounted = true;
        const FString Name = Mount + TEXT("BP_Splinemesh"); FLinkerInstancingContext Context(true);
        Context.AddPackageMapping(TEXT("/Game/ArchvisProject/Blueprints/BP_Splinemesh"), FName(*Name));
        Package.Reset(LoadPackage(nullptr, *Name, LOAD_None, nullptr, &Context));
        if (!Test.TestNotNull(TEXT("Native loader instances the copied package"), Package.Get())) { return Finish(); }
        Test.TestEqual(TEXT("Only the private package is loaded for formatting"), Package->GetName(), Name);
        Blueprint.Reset(Cast<UBlueprint>(Package->FindAssetInPackage()));
        if (!Test.TestNotNull(TEXT("Installed template loads as a real Blueprint"), Blueprint.Get()) || !Compile()) { return Finish(); }
        Graph = FBlueprintEditorUtils::FindUserConstructionScript(Blueprint.Get());
        if (!Test.TestTrue(TEXT("Authored construction graph is nontrivial"), Graph && Graph->Nodes.Num() > 4)) { return Finish(); }
        for (UEdGraphNode* Node : Graph->Nodes) { if (Node->IsA<UK2Node_FunctionEntry>()) { Entry = Node; break; } }
        if (!Test.TestNotNull(TEXT("Construction script retains its entry node"), Entry)) { return Finish(); }
        PrimeTooltips();
        auto* Settings = GetMutableDefault<UGlooPrintSettings>(); OriginalSettings = Settings->GetLayoutSettings(); OriginalStyle = Settings->WireStyle;
        bOriginalEnabled = Settings->bFormattingEnabled; bRestoreSettings = true;
        Settings->HorizontalSpacing = 96; Settings->VerticalSpacing = 48; Settings->CommentPadding = 32;
        Settings->WireStyle = EGlooPrintWireStyle::Rounded90; Settings->bFormattingEnabled = true; Settings->NotifyChanged();
        Editor = SNew(SGraphEditor).GraphToEdit(Graph).IsEditable(true);
        Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint authored spline mesh Blueprint"))).ClientSize(FVector2f(1550, 1000))[Editor.ToSharedRef()];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef()); Editor->SetNodeSelection(Entry, true); Editor->ZoomToFit(false);
        GLog->AddOutputDevice(this); bListening = true;
        Test.AddInfo(TEXT("Private authored template artifacts: ") + Directory); return false;
    }
    bool Compile()
    {
        FCompilerResultsLog Result; FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &Result);
        return Test.TestEqual(TEXT("The real spline Blueprint compiles without errors"), Result.NumErrors, 0);
    }
    void PrimeTooltips()
    {
        for (UEdGraphNode* Node : Graph->Nodes) { for (auto* Pin : Node->Pins) { FString Text; Node->GetPinHoverText(*Pin, Text); } }
    }
    void BeginFormat()
    {
        Changed = -1; bAwaitingFormat = true; LastMessage.Reset();
        auto& Slate = FSlateApplication::Get(); Slate.SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
        Test.TestTrue(TEXT("Focused F is handled for the real template"), Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0)));
        Frames = 0;
    }
    void CheckContext()
    {
        Test.TestEqual(TEXT("F preserves the camera offset at completion"), CompletedView, View);
        Test.TestEqual(TEXT("F preserves zoom at completion"), CompletedZoom, Zoom);
        Test.TestTrue(TEXT("F preserves the selected entry anchor"), Editor->GetSelectedNodes().Num() == 1 && Editor->GetSelectedNodes().Contains(Entry) &&
            FIntPoint(Entry->NodePosX, Entry->NodePosY) == Anchor);
    }
    void CheckProperties()
    {
        const auto Current = DescribeNodes(*Graph); Test.TestEqual(TEXT("All authored property identities remain"), Current.Num(), Properties.Num());
        for (const auto& Pair : Properties)
        {
            if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY"))) { continue; }
            bool bCommentSize = false;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node->IsA<UEdGraphNode_Comment>() && (Pair.Key == Node->GetName() + TEXT(".NodeWidth") || Pair.Key == Node->GetName() + TEXT(".NodeHeight"))) { bCommentSize = true; break; }
            }
            if (!bCommentSize) { const auto* Value = Current.Find(Pair.Key); Test.TestTrue(Pair.Key + TEXT(" survives formatting"), Value && *Value == Pair.Value); }
        }
    }
    void CheckRoutes(const FFormatPlan& Cold, const FRouteSet& Actual)
    {
        Test.TestEqual(TEXT("Formatted template retains all original connections"), Actual.Wires.Num(), Plan.Snapshot.Edges.Num());
        Test.TestEqual(TEXT("Formatted template has no native fallback"), Actual.FallbackCount, 0);
        for (const auto& Pair : Cold.Routes.Wires)
        {
            const auto* Route = Actual.Wires.Find(Pair.Key);
            Test.TestTrue(TEXT("Authored pin pairs keep exact cold and reordered paths"), Route && Route->Points == Pair.Value.Points);
        }
    }
    void RecordMetrics(const TCHAR* Stage, const FLayoutGraph& Snapshot, const FRouteSet& Routes)
    {
        FString Details = TEXT("node\tobject\tlabel\tx\ty\twidth\theight\tcomment\n");
        TMap<FGuid, UEdGraphNode*> NativeNodes;
        for (UEdGraphNode* Node : Graph->Nodes) { NativeNodes.Add(Node->NodeGuid, Node); }
        for (int32 I = 0; I < Snapshot.Nodes.Num(); ++I)
        {
            const auto& Node = Snapshot.Nodes[I]; const auto* Native = NativeNodes.FindChecked(Node.Geometry.Id);
            FString Label = Native->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Replace(TEXT("\t"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" | "));
            Details += FString::Printf(TEXT("%d\t%s\t%s\t%d\t%d\t%.3f\t%.3f\t%d\n"), I, *Native->GetName(), *Label,
                Node.Geometry.Position.X, Node.Geometry.Position.Y, Node.Geometry.BodySize.X, Node.Geometry.BodySize.Y, Node.bComment);
        }
        Test.TestTrue(TEXT("Save native template node diagnostics"), FFileHelper::SaveStringToFile(Details, *(Directory / (FString(Stage) + TEXT("-nodes.tsv")))));
        Details = TEXT("edge\tkind\tfrom_node\tfrom_pin\tto_node\tto_pin\tfrom_x\tfrom_y\tto_x\tto_y\n");
        for (int32 I = 0; I < Snapshot.Edges.Num(); ++I)
        {
            const auto& Edge = Snapshot.Edges[I]; const auto& A = Snapshot.Pins[Edge.From]; const auto& B = Snapshot.Pins[Edge.To];
            const auto* From = NativeNodes.FindChecked(Snapshot.Nodes[A.Node].Geometry.Id);
            const auto* To = NativeNodes.FindChecked(Snapshot.Nodes[B.Node].Geometry.Id);
            const FVector2f Start = FVector2f(Snapshot.Nodes[A.Node].Geometry.Position) + A.Offset.GetValue();
            const FVector2f End = FVector2f(Snapshot.Nodes[B.Node].Geometry.Position) + B.Offset.GetValue();
            Details += FString::Printf(TEXT("%d\t%d\t%d\t%s\t%d\t%s\t%.3f\t%.3f\t%.3f\t%.3f\n"), I, int32(Edge.Kind), A.Node,
                *From->Pins[A.Ordinal]->PinName.ToString(), B.Node, *To->Pins[B.Ordinal]->PinName.ToString(), Start.X, Start.Y, End.X, End.Y);
        }
        Test.TestTrue(TEXT("Save native template pin diagnostics"), FFileHelper::SaveStringToFile(Details, *(Directory / (FString(Stage) + TEXT("-edges.tsv")))));
        FBox2f Nodes(ForceInit), Envelope(ForceInit); int32 Bends = 0, Execution = 0, Aligned = 0; double Length = 0;
        for (const auto& Node : Snapshot.Nodes) { Nodes += FVector2f(Node.Geometry.Position); Nodes += FVector2f(Node.Geometry.Position) + Node.Geometry.BodySize; }
        Envelope = Nodes;
        for (const auto& Pair : Routes.Wires)
        {
            const auto& R = Pair.Value; Bends += FMath::Max(0, R.Points.Num() - 2); Length += R.Length;
            if (!R.Points.IsEmpty()) { Envelope += R.Bounds.Min; Envelope += R.Bounds.Max; }
        }
        for (const auto& Edge : Snapshot.Edges)
        {
            if (Edge.Kind != ELinkKind::Execution) { continue; } ++Execution;
            const auto& A = Snapshot.Pins[Edge.From]; const auto& B = Snapshot.Pins[Edge.To];
            if (A.Offset.IsSet() && B.Offset.IsSet() && Snapshot.Nodes[A.Node].Geometry.Position.Y + A.Offset->Y == Snapshot.Nodes[B.Node].Geometry.Position.Y + B.Offset->Y) { ++Aligned; }
        }
        Test.TestEqual(TEXT("Authored spline execution chain stays exactly aligned"), Aligned, Execution);
        const FVector2f N = Nodes.Max - Nodes.Min, E = Envelope.Max - Envelope.Min;
        const FString Row = FString::Printf(TEXT("%s,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f\n"), Stage, Snapshot.Nodes.Num(), Routes.Wires.Num(), Routes.FallbackCount,
            Bends, Aligned, Execution, Length, N.X, N.Y, E.X, E.Y);
        Metrics += Row; Test.AddInfo(Row.TrimEnd());
    }
    void Capture(const TCHAR* Stage)
    {
        FVector2f CaptureView; float CaptureZoom; Editor->GetViewLocation(CaptureView, CaptureZoom);
        const FVector2f TopLeft = (CaptureBounds.Min - CaptureView) * CaptureZoom;
        const FVector2f BottomRight = (CaptureBounds.Max - CaptureView) * CaptureZoom;
        const FVector2f Available = Editor->GetGraphPanel()->GetCachedGeometry().GetLocalSize();
        Test.TestTrue(TEXT("Native capture includes the complete authored and formatted envelopes"),
            TopLeft.X >= 0 && TopLeft.Y >= 0 && BottomRight.X <= Available.X && BottomRight.Y <= Available.Y);
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture the actual authored Blueprint viewport"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save authored Blueprint viewport"), FFileHelper::SaveArrayToFile(Png, *(Directory / (FString(Stage) + TEXT(".png")))));
        }
    }
    bool Finish() { Restore(); return true; }
    void Restore()
    {
        bAwaitingFormat = false;
        if (bListening) { GLog->RemoveOutputDevice(this); bListening = false; }
        if (Window) { Window->RequestDestroyWindow(); } Window.Reset(); Editor.Reset();
        if (Package.IsValid()) { Package->SetDirtyFlag(false); }
        if (bMounted) { FPackageName::UnRegisterMountPoint(Mount, Directory); bMounted = false; }
        if (bRestoreSettings)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>(); Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing;
            Settings->VerticalSpacing = OriginalSettings.VerticalSpacing; Settings->CommentPadding = OriginalSettings.CommentPadding;
            Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged(); bRestoreSettings = false;
        }
    }
    FAutomationTestBase& Test;
    TStrongObjectPtr<UPackage> Package;
    TStrongObjectPtr<UBlueprint> Blueprint;
    UEdGraph* Graph = nullptr;
    UEdGraphNode* Entry = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FFormatPlan Plan;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    TMap<FString, FString> Properties;
    TMap<UEdGraph*, TArray<uint8>> OtherGraphs;
    TArray<uint8> SourceBytes, Before, After, Defaults;
    FString SourcePath, CopyPath, Directory, Mount, LastMessage;
    FString Metrics = TEXT("stage,nodes,links,fallbacks,bends,aligned_execution,execution_links,route_length,node_width,node_height,envelope_width,envelope_height\n");
    FVector2f View, CompletedView;
    FBox2f CaptureBounds{ForceInit};
    FIntPoint Anchor;
    float Zoom = 0, CompletedZoom = 0;
    double Deadline = 0;
    int32 Phase = 0, Frames = 0, Queue = 0, Changed = -1;
    bool bInitialized = false, bMounted = false, bRestoreSettings = false, bOriginalEnabled = true, bListening = false, bAwaitingFormat = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTemplateSplineMeshTest, "GlooPrint.Editor.TemplateSplineMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTemplateSplineMeshTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FTemplateBlueprintCheck(*this)); return true;
}
}
#endif

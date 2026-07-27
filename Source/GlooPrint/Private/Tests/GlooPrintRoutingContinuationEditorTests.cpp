#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintEditor.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "SGraphPanel.h"
#include "ScopedTransaction.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FRouteContinuationCheck final : public IAutomationLatentCommand
{
public:
    explicit FRouteContinuationCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FRouteContinuationCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            OriginalStyle = GetDefault<UGlooPrintSettings>()->WireStyle;
            bRestore = true;
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Rounded90;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false);
            auto* Source = Fixture->Add<UK2Node_ExecutionSequence>({0, 0});
            Target = Fixture->Add<UK2Node_ExecutionSequence>({6000, 5000});
            for (int32 I = 2; I < LinkCount; ++I) { Source->AddInputPin(); }
            for (int32 I = 0; I < LinkCount; ++I)
            {
                if (!Test.TestTrue(TEXT("Create real native execution fan-in"),
                    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Source->GetThenPinGivenIndex(I),
                        Target->FindPinChecked(UEdGraphSchema_K2::PN_Execute)))) { return Finish(); }
            }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint cancellable native wire rebuild")))
                .ClientSize(FVector2f(1400, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-100, 0), 0.15f);
            Deadline = FPlatformTime::Seconds() + 45;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Native routing continuation did not finish in 45 seconds.")); return Finish(); }
        if (Phase == 5)
        {
            if (++Frames < 10) { return false; }
            Test.TestFalse(TEXT("Closing a graph releases its pending routing job and cache"), WeakCache.IsValid());
            Test.TestTrue(TEXT("Closing during routing leaves exact graph state unchanged"), SerializeNodes(*Fixture->Graph) == Before);
            return Finish();
        }
        auto* Panel = Editor->GetGraphPanel();
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache) { return false; }
        if (Phase == 0)
        {
            if (!Cache->HasPendingRouting()) { return false; }
            Test.TestFalse(TEXT("Incomplete native rebuild is not ready"), Cache->IsReady());
            Test.TestTrue(TEXT("Incomplete native rebuild exposes no partial routes"), Cache->GetRoutes().Wires.IsEmpty());
            Before = SerializeNodes(*Fixture->Graph);
            Queue = GEditor->Trans->GetQueueLength();
            {
                FScopedTransaction Transaction(FText::FromString(TEXT("Move target during native routing")));
                Target->Modify(); Target->NodePosY += 160;
                Fixture->Graph->NotifyGraphChanged();
            }
            Changed = SerializeNodes(*Fixture->Graph);
            Test.TestFalse(TEXT("Graph edit immediately discards the old routing job"), Cache->HasPendingRouting());
            Test.TestFalse(TEXT("Stale graph keeps native fallback until a new job finishes"), Cache->IsReady());
            Builds = Cache->GetBuildCount(); Phase = 1; return false;
        }
        if (Phase == 1)
        {
            if (!Cache->IsReady()) { return false; }
            Test.TestTrue(TEXT("Edit starts a fresh capture instead of resuming the old snapshot"), Cache->GetBuildCount() > Builds);
            Test.TestEqual(TEXT("Finished cache retains every native connection"), Cache->GetRoutes().Wires.Num(), LinkCount);
            Test.TestTrue(TEXT("Routing leaves all serialized values unchanged"), SerializeNodes(*Fixture->Graph) == Changed);
            Test.TestEqual(TEXT("Background routing adds no undo entry"), GEditor->Trans->GetQueueLength(), Queue + 1);
            FLayoutGraph Graph; FRouteSet Expected; FString Reason;
            FMeasurementOptions Options; Options.PinVisibility = Panel->GetPinVisibility();
            const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
            if (!Test.TestTrue(TEXT("Read current native graph after resumed routing"),
                CaptureGraphForRouting(Fixture->Graph, Scale, Graph, Reason, Options) && ComputeRoutes(Graph, Expected, Reason)))
            {
                Test.AddError(Reason); return Finish();
            }
            for (const auto& Pair : Expected.Wires)
            {
                const auto* Actual = Cache->GetRoutes().Wires.Find(Pair.Key);
                Test.TestTrue(TEXT("Only the current graph's exact route is published"), Actual && Actual->Points == Pair.Value.Points && Actual->Fallback == Pair.Value.Fallback);
            }
            Test.TestTrue(TEXT("Undo of the user's edit succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Undo restores exact state despite intervening background routing"), SerializeNodes(*Fixture->Graph) == Before);
            Phase = 2; return false;
        }
        if (Phase == 2)
        {
            if (!Cache->HasPendingRouting()) { return false; }
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Native;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Test.TestFalse(TEXT("Native style cancels pending custom computation"), Cache->HasPendingRouting());
            Builds = Cache->GetBuildCount(); Frames = 0; Phase = 3; return false;
        }
        if (Phase == 3)
        {
            if (++Frames < 10) { return false; }
            Test.TestEqual(TEXT("Canceled native-style work remains idle"), Cache->GetBuildCount(), Builds);
            Test.TestTrue(TEXT("Style cancellation leaves exact graph state unchanged"), SerializeNodes(*Fixture->Graph) == Before);
            GetMutableDefault<UGlooPrintSettings>()->WireStyle = EGlooPrintWireStyle::Rounded90;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Phase = 4; return false;
        }
        if (Phase == 4)
        {
            if (!Cache->HasPendingRouting()) { return false; }
            WeakCache = Cache;
            Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset();
            Frames = 0; Phase = 5; return false;
        }
        return false;
    }
private:
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        if (Window) { Window->RequestDestroyWindow(); }
        Window.Reset(); Editor.Reset();
        GetMutableDefault<UGlooPrintSettings>()->WireStyle = OriginalStyle;
        GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
    }
    bool Finish() { Restore(); return true; }
    static constexpr int32 LinkCount = 512;
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    UK2Node_ExecutionSequence* Target = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TWeakPtr<FRouteCache> WeakCache;
    TArray<uint8> Before, Changed;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    int32 Phase = 0, Frames = 0, Queue = 0, Builds = 0;
    double Deadline = 0;
    bool bRestore = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRouteContinuationEditorTest, "GlooPrint.Editor.RoutingContinuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRouteContinuationEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FRouteContinuationCheck(*this)); return true;
}

class FFormatContinuationCheck final : public IAutomationLatentCommand
{
public:
    explicit FFormatContinuationCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual ~FFormatContinuationCheck() { Restore(); }
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            auto* Settings = GetMutableDefault<UGlooPrintSettings>();
            OriginalStyle = Settings->WireStyle; bOriginalEnabled = Settings->bFormattingEnabled; bRestore = true;
            Settings->WireStyle = EGlooPrintWireStyle::Native; Settings->bFormattingEnabled = true; Settings->NotifyChanged();
            Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false);
            Source = Fixture->Add<UK2Node_ExecutionSequence>({0, 0});
            Target = Fixture->Add<UK2Node_ExecutionSequence>({6000, 5000});
            for (int32 I = 2; I < 512; ++I) { Source->AddInputPin(); }
            for (int32 I = 0; I < 512; ++I)
            {
                if (!Test.TestTrue(TEXT("Create native fan for cancellable F"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(
                    Source->GetThenPinGivenIndex(I), Target->FindPinChecked(UEdGraphSchema_K2::PN_Execute)))) { return Finish(); }
            }
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint cancellable F")))
                .ClientSize(FVector2f(1400, 1000))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetNodeSelection(Source, true);
            Editor->SetViewLocation(FVector2f(-100, 0), 0.15f);
            Deadline = FPlatformTime::Seconds() + 45;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Native F continuation did not finish in 45 seconds.")); return Finish(); }
        if (++Frames < 12) { return false; }
        if (Phase == 0)
        {
            FFormatPlan Reference; FString Reason;
            const float Scale = Window->GetDPIScaleFactor() * Slate.GetApplicationScale();
            if (!Test.TestTrue(TEXT("Capture complete reference plan with native measurements"),
                PlanFormatGraph(Fixture->Graph, Scale, {Source->NodeGuid}, Reference, Reason))) { Test.AddError(Reason); return Finish(); }
            Plan = MoveTemp(Reference);
            Before = SerializeNodes(*Fixture->Graph); Queue = GEditor->Trans->GetQueueLength();
            BeginFormat();
            Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0));
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            CheckUnchanged(TEXT("Escape during F"));
            BeginFormat();
            Target->NodeComment = TEXT("Edited while the layout was pending");
            Before = SerializeNodes(*Fixture->Graph);
            Phase = 2; Frames = 0; return false;
        }
        if (Phase == 2)
        {
            CheckUnchanged(TEXT("Unnotified node edit during F"));
            BeginFormat();
            Editor->ClearSelectionSet();
            Phase = 3; Frames = 0; return false;
        }
        if (Phase == 3)
        {
            CheckUnchanged(TEXT("Selection changed during F"));
            Editor->SetNodeSelection(Source, true);
            BeginFormat();
            GetMutableDefault<UGlooPrintSettings>()->bFormattingEnabled = false;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            Phase = 4; Frames = 0; return false;
        }
        if (Phase == 4)
        {
            CheckUnchanged(TEXT("Settings changed during F"));
            GetMutableDefault<UGlooPrintSettings>()->bFormattingEnabled = true;
            GetMutableDefault<UGlooPrintSettings>()->NotifyChanged();
            BeginFormat();
            Phase = 5; Frames = 12; return false;
        }
        if (Phase == 5)
        {
            if (FIntPoint(Target->NodePosX, Target->NodePosY) == FIntPoint(6000, 5000))
            {
                Test.TestEqual(TEXT("Pending planning never opens a transaction"), GEditor->Trans->GetQueueLength(), Queue);
                Test.TestTrue(TEXT("Pending computation preserves all graph bytes"), SerializeNodes(*Fixture->Graph) == Before);
                Test.TestTrue(TEXT("Duplicate F remains handled"), PressF());
                Test.TestTrue(TEXT("Key repeat remains handled"), PressF(true));
                return false;
            }
            Test.TestEqual(TEXT("Completed F applies exactly one transaction"), GEditor->Trans->GetQueueLength(), Queue + 1);
            for (UEdGraphNode* Node : Fixture->Graph->Nodes)
            {
                const int32 I = Plan.Snapshot.Nodes.IndexOfByPredicate([Node](const auto& N) { return N.Geometry.Id == Node->NodeGuid; });
                Test.TestEqual(TEXT("Resumed F matches reference positions"), FIntPoint(Node->NodePosX, Node->NodePosY), Plan.Layout.Positions[I]);
            }
            Test.TestTrue(TEXT("Selected source remains the anchor"), Source->NodePosX == 0 && Source->NodePosY == 0 && Editor->GetSelectedNodes().Contains(Source));
            After = SerializeNodes(*Fixture->Graph);
            Test.TestTrue(TEXT("Resumed F undo succeeds"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Undo restores exact graph including the intervening comment edit"), SerializeNodes(*Fixture->Graph) == Before);
            Test.TestTrue(TEXT("Resumed F redo succeeds"), GEditor->RedoTransaction());
            Test.TestTrue(TEXT("Redo restores exact formatted result"), SerializeNodes(*Fixture->Graph) == After);
            Before = After; Queue = GEditor->Trans->GetQueueLength();
            Test.TestTrue(TEXT("No-op F is accepted"), PressF());
            Phase = 6; Frames = 0; return false;
        }
        if (Phase == 6)
        {
            CheckUnchanged(TEXT("Resumed no-op F"));
            Test.TestTrue(TEXT("F before closing its panel is accepted"), PressF());
            Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset();
            Phase = 7; Frames = 0; return false;
        }
        CheckUnchanged(TEXT("Panel closed during F"));
        return Finish();
    }
private:
    bool PressF(bool bRepeat = false) { return FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, bRepeat, 0, 0)); }
    void BeginFormat()
    {
        FSlateApplication::Get().SetKeyboardFocus(Editor->GetGraphPanel()->AsShared(), EFocusCause::SetDirectly);
        Test.TestTrue(TEXT("Actual F starts a slow native graph plan"), PressF());
        CheckUnchanged(TEXT("F yields before applying its pending plan"));
    }
    void CheckUnchanged(const FString& Context)
    {
        Test.TestTrue(Context + TEXT(" leaves exact graph state unchanged"), SerializeNodes(*Fixture->Graph) == Before);
        Test.TestEqual(Context + TEXT(" adds no undo entry"), GEditor->Trans->GetQueueLength(), Queue);
    }
    bool Finish() { Restore(); return true; }
    void Restore()
    {
        if (!bRestore) { return; } bRestore = false;
        if (Window) { Window->RequestDestroyWindow(); }
        Window.Reset(); Editor.Reset();
        auto* Settings = GetMutableDefault<UGlooPrintSettings>();
        Settings->WireStyle = OriginalStyle; Settings->bFormattingEnabled = bOriginalEnabled; Settings->NotifyChanged();
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    UK2Node_ExecutionSequence* Source = nullptr;
    UK2Node_ExecutionSequence* Target = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FFormatPlan Plan;
    TArray<uint8> Before, After;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    int32 Phase = 0, Frames = 0, Queue = 0;
    double Deadline = 0;
    bool bRestore = false, bOriginalEnabled = true;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormatContinuationEditorTest, "GlooPrint.Editor.FormatContinuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFormatContinuationEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FFormatContinuationCheck(*this)); return true;
}
}
#endif

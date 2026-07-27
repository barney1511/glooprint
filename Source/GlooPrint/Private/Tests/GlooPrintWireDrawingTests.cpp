#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphPanel.h"
#include "ScopedTransaction.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FWireCheck final : public IAutomationLatentCommand
{
public:
    explicit FWireCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            OriginalCursor = Slate.GetCursorPos();
            Fixture = MakeUnique<FFixture>();
            Output = Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
            Input = Output->LinkedTo[0];
            Input->GetOwningNode()->SetPosition({700, 280});
            Fixture->Print->SetPosition({320, 30});
            Key = {Output->GetOwningNode()->NodeGuid, Output->PinId, Input->GetOwningNode()->NodeGuid, Input->PinId};
            FString NativeTooltip;
            Fixture->Print->GetPinHoverText(*Fixture->Print->FindPinChecked(TEXT("InString")), NativeTooltip);
            Test.TestTrue(TEXT("Native function tooltip is available before opening the graph"), !NativeTooltip.IsEmpty());
            Before = SerializeNodes(*Fixture->Graph);
            BeforeProperties = DescribeNodes(*Fixture->Graph);
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint rounded routes"))).ClientSize(FVector2f(1200, 850))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-100, -200), 1.f);
            Deadline = FPlatformTime::Seconds() + 30;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Wire fixture did not settle within 30 seconds.")); return Finish(); }
        if (Phase == 4)
        {
            if (++Frames < 8) { return false; }
            Test.TestFalse(TEXT("Closing the graph releases its route cache"), WeakCache.IsValid());
            return Finish();
        }
        SGraphPanel* Panel = Editor->GetGraphPanel();
        auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        const auto* Route = Cache->GetRoutes().Wires.Find(Key);
        if (!Test.TestTrue(TEXT("Open graph creates a route without pressing F"), Route && !Route->Curves.IsEmpty())) { return Finish(); }
        if (Phase == 0)
        {
            if (++Frames < 8) { return false; }
            if (!Test.TestTrue(TEXT("Automatic routing preserves every serialized node/pin byte"), Before == SerializeNodes(*Fixture->Graph)))
            {
                ReportNodeDifferences(Test, BeforeProperties, *Fixture->Graph);
            }
            OriginalPoints = Route->Points;
            Builds = Cache->GetBuildCount();
            Hover(*Panel, *Route);
            Phase = 1; Frames = 0; return false;
        }
        if (Phase == 1)
        {
            if (++Frames < 8) { return false; }
            UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
            Test.TestTrue(TEXT("Native hover finds the visible routed wire"), Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B));
            Test.TestTrue(TEXT("Visible routed wire identifies its original pin pair"), (A == Output && B == Input) || (A == Input && B == Output));
            Test.TestEqual(TEXT("Idle paints do not rebuild routes"), Cache->GetBuildCount(), Builds);
            TArray<FColor> Pixels; FIntVector Size;
            if (Test.TestTrue(TEXT("Capture rounded route fixture"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
            {
                TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
                Test.TestTrue(TEXT("Save rounded wire screenshot"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("GlooPrint-RoundedWires.png"))));
            }
            const int32 NodesBeforeClick = Fixture->Graph->Nodes.Num();
            const FPointerEvent Click(0, Mouse, Mouse, TSet<FKey>(), EKeys::LeftMouseButton, 0, FModifierKeysState());
            Panel->OnMouseButtonDoubleClick(Panel->GetCachedGeometry(), Click);
            if (!Test.TestEqual(TEXT("Double-click on the rendered wire inserts one native reroute"), Fixture->Graph->Nodes.Num(), NodesBeforeClick + 1)) { return Finish(); }
            auto* Knot = Output->LinkedTo.IsEmpty() ? nullptr : Cast<UK2Node_Knot>(Output->LinkedTo[0]->GetOwningNode());
            Test.TestTrue(TEXT("Reroute insertion splits the identified pin pair"), Knot && Knot->GetOutputPin()->LinkedTo.Contains(Input));
            Test.TestTrue(TEXT("Reroute insertion remains one native undo"), GEditor->UndoTransaction());
            Phase = 5; Frames = 0; return false;
        }
        if (Phase == 5)
        {
            if (++Frames < 8) { return false; }
            Output = Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
            Input = Output->LinkedTo[0];
            Test.TestTrue(TEXT("Undoing reroute insertion restores every original graph value"), Before == SerializeNodes(*Fixture->Graph));
            Builds = Cache->GetBuildCount();
            Editor->SetViewLocation(FVector2f(-60, -140), 0.75f);
            Phase = 6; Frames = 0; return false;
        }
        if (Phase == 6)
        {
            if (++Frames < 8) { return false; }
            Hover(*Panel, *Route);
            Phase = 7; Frames = 0; return false;
        }
        if (Phase == 7)
        {
            if (++Frames < 8) { return false; }
            UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
            Test.TestTrue(TEXT("Pan and zoom preserve visible-path hover"), Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B) &&
                ((A == Output && B == Input) || (A == Input && B == Output)));
            Test.TestEqual(TEXT("Pan and zoom reuse graph-coordinate routes"), Cache->GetBuildCount(), Builds);
            {
                const FScopedTransaction Transaction(FText::FromString(TEXT("Move wire obstacle")));
                Fixture->Print->Modify();
                Test.TestFalse(TEXT("Modifying an unrelated obstacle immediately invalidates this wire"), Cache->IsReady());
                Fixture->Print->SetPosition({350, -60});
            }
            Phase = 2; Frames = 0; return false;
        }
        if (Phase == 2)
        {
            if (++Frames < 8) { return false; }
            Test.TestTrue(TEXT("Obstacle edit rebuilds routes outside paint"), Cache->GetBuildCount() > Builds);
            Test.TestTrue(TEXT("Native undo of obstacle movement succeeds"), GEditor->UndoTransaction());
            Test.TestFalse(TEXT("Undo invalidates routes"), Cache->IsReady());
            Phase = 3; Frames = 0; return false;
        }
        if (Phase == 3)
        {
            if (++Frames < 8) { return false; }
            Test.TestTrue(TEXT("Undo restores the original deterministic wire"), Route->Points == OriginalPoints);
            Test.TestTrue(TEXT("Route rebuild after undo changes no graph values"), Before == SerializeNodes(*Fixture->Graph));
            Test.TestTrue(TEXT("Obstacle move also supports native redo"), GEditor->RedoTransaction());
            WeakCache = Cache;
            Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset();
            Phase = 4; Frames = 0;
            return false;
        }
        return false;
    }
    bool Finish()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        FSlateApplication::Get().SetCursorPos(OriginalCursor);
        Editor.Reset(); Window.Reset(); return true;
    }
private:
    void Hover(SGraphPanel& Panel, const FWireRoute& Route)
    {
        const FVector2f GraphPoint = EvaluateRoute(Route, Route.Length * 0.5f);
        Mouse = Panel.GetCachedGeometry().LocalToAbsolute((GraphPoint - FVector2f(Panel.GetViewOffset())) * Panel.GetZoomAmount());
        MoveMouseOverGraph(Test, Window.ToSharedRef(), Panel, Mouse);
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    TWeakPtr<FRouteCache> WeakCache;
    UEdGraphPin* Output = nullptr;
    UEdGraphPin* Input = nullptr;
    FRouteKey Key;
    TArray<uint8> Before;
    TMap<FString, FString> BeforeProperties;
    TArray<FVector2f> OriginalPoints;
    FVector2f Mouse;
    FVector2D OriginalCursor;
    double Deadline = 0;
    int32 Phase = 0, Frames = 0, Builds = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWireDrawingTest, "GlooPrint.Editor.RoutedWires",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWireDrawingTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FWireCheck(*this)); return true;
}
}
#endif

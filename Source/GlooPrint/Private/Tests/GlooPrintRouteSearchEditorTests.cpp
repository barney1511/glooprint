#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintWireDrawing.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphPanel.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
class FSearchWireCheck final : public IAutomationLatentCommand
{
public:
    explicit FSearchWireCheck(FAutomationTestBase& InTest) : Test(InTest) {}
    virtual bool Update() override
    {
        auto& Slate = FSlateApplication::Get();
        if (!Fixture)
        {
            OriginalCursor = Slate.GetCursorPos();
            Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false);
            Fixture->Branch = Fixture->Add<UK2Node_IfThenElse>({0, 300});
            auto* Target = Fixture->Add<UK2Node_ExecutionSequence>({1400, 300});
            Output = Fixture->Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
            Input = Target->FindPinChecked(UEdGraphSchema_K2::PN_Execute);
            GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Output, Input);
            Key = {Fixture->Branch->NodeGuid, Output->PinId, Target->NodeGuid, Input->PinId};
            Fixture->Print = NewObject<UK2Node_CallFunction>(Fixture->Graph);
            Fixture->Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
            Fixture->Initialize(*Fixture->Print, {680, 180});
            Fixture->Print->PostPlacedNewNode();
            Fixture->Print->AdvancedPinDisplay = ENodeAdvancedPins::Shown;
            Fixture->Print->FindPinChecked(TEXT("InString"))->DefaultValue = TEXT("A measured body obstacle");
            Gate({100, 260}, 400, TEXT("Upper source gate"));
            Gate({100, 440}, 400, TEXT("Lower source gate"));
            Gate({1120, 260}, 450, TEXT("Upper destination gate"));
            Gate({1120, 470}, 450, TEXT("Lower destination gate"));
            Before = SerializeNodes(*Fixture->Graph);
            Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
            Window = SNew(SWindow).Title(FText::FromString(TEXT("GlooPrint searched route"))).ClientSize(FVector2f(1200, 850))[Editor.ToSharedRef()];
            Slate.AddWindow(Window.ToSharedRef());
            Editor->SetViewLocation(FVector2f(-100, 100), 0.65f);
            Deadline = FPlatformTime::Seconds() + 30;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test.AddError(TEXT("Searched route fixture did not settle in 30 seconds.")); return Finish(); }
        auto* Panel = Editor->GetGraphPanel();
        const auto Cache = Panel->GetMetaData<FRouteCache>();
        if (!Cache || !Cache->IsReady()) { return false; }
        const auto* Route = Cache->GetRoutes().Wires.Find(Key);
        if (!Test.TestTrue(TEXT("Native fixture finds a custom searched route"), Route && Route->Method == ERouteMethod::Search)) { return Finish(); }
        if (++Frames < 8) { return false; }
        if (Phase == 0)
        {
            Test.TestEqual(TEXT("Native obstacle fixture has no fallback"), Cache->GetRoutes().FallbackCount, 0);
            Test.TestTrue(TEXT("Search leaves all serialized native node/pin data unchanged"), Before == SerializeNodes(*Fixture->Graph));
            Test.AddInfo(FString::Printf(TEXT("Native search: %d expanded states, %d segment checks, %d bends."),
                Route->Search.ExpandedStates, Route->Search.SegmentChecks, Route->Points.Num() - 2));
            const FVector2f P = EvaluateRoute(*Route, Route->Length * 0.5f);
            Mouse = Panel->GetCachedGeometry().LocalToAbsolute((P - FVector2f(Panel->GetViewOffset())) * Panel->GetZoomAmount());
            MoveMouseOverGraph(Test, Window.ToSharedRef(), *Panel, Mouse);
            Builds = Cache->GetBuildCount(); Phase = 1; Frames = 0;
            return false;
        }
        UEdGraphPin* A = nullptr; UEdGraphPin* B = nullptr;
        Test.TestTrue(TEXT("Native hover identifies the searched wire's original pins"),
            Panel->GetPreviousFrameSplineOverlap().GetPins(*Panel, A, B) &&
            ((A == Output && B == Input) || (A == Input && B == Output)));
        Test.TestEqual(TEXT("Painting the searched wire does not repeat A*"), Cache->GetBuildCount(), Builds);
        TArray<FColor> Pixels; FIntVector Size;
        if (Test.TestTrue(TEXT("Capture searched native route"), Slate.TakeScreenshot(Editor.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
            Test.TestTrue(TEXT("Save searched route screenshot"), FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("GlooPrint-SearchedRoute.png"))));
        }
        return Finish();
    }
private:
    void Gate(FVector2f Position, int32 Width, const TCHAR* Label)
    {
        auto* Comment = Fixture->Add<UEdGraphNode_Comment>(Position);
        Comment->NodeWidth = Width; Comment->NodeHeight = 110; Comment->NodeComment = Label;
    }
    bool Finish()
    {
        if (Window) { Window->RequestDestroyWindow(); }
        FSlateApplication::Get().SetCursorPos(OriginalCursor);
        Editor.Reset(); Window.Reset(); return true;
    }
    FAutomationTestBase& Test;
    TUniquePtr<FFixture> Fixture;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    UEdGraphPin* Output = nullptr;
    UEdGraphPin* Input = nullptr;
    FRouteKey Key;
    TArray<uint8> Before;
    FVector2D OriginalCursor;
    FVector2f Mouse;
    double Deadline = 0;
    int32 Frames = 0, Phase = 0, Builds = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchWireEditorTest, "GlooPrint.Editor.SearchedWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSearchWireEditorTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FSearchWireCheck(*this)); return true;
}
}
#endif

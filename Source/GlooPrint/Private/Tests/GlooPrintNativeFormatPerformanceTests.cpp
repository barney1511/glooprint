#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"
#include "GlooPrintMeasurementCache.h"
#include "GlooPrintSettings.h"
#include "GlooPrintWireDrawing.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "SGraphPanel.h"
#include "Widgets/SWindow.h"

namespace GlooPrint::Tests
{
namespace
{
class FFormatCompletion final : public FOutputDevice
{
public:
    virtual bool CanBeUsedOnAnyThread() const override { return true; }
    virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
    virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
    {
        if (!IsInGameThread() || !bArmed || Category != FName(TEXT("LogGlooPrintEditor"))) { return; }
        static constexpr TCHAR Prefix[] = TEXT("Formatted graph: ");
        if (FCString::Strncmp(Message, Prefix, UE_ARRAY_COUNT(Prefix) - 1) != 0) { LastMessage = Message; return; }
        CompletedAt = FPlatformTime::Seconds(); CompletedFrame = GFrameCounter;
        Changed = FCString::Atoi(Message + UE_ARRAY_COUNT(Prefix) - 1); bArmed = false;
        TRACE_BOOKMARK(TEXT("GlooPrintNativeF complete %s changed=%d"), *Label, Changed);
    }
    void Arm(FString InLabel)
    {
        Label = MoveTemp(InLabel); LastMessage.Reset(); Changed = -1; CompletedAt = 0; bArmed = true;
    }
    FString Label, LastMessage;
    double CompletedAt = 0;
    uint64 CompletedFrame = 0;
    int32 Changed = -1;
    bool bArmed = false;
};
}

class FNativeFormatBenchmark final : public IAutomationLatentCommand
{
public:
    FNativeFormatBenchmark(FAutomationTestBase& InTest, int32 InCount, FString InFamily)
        : Test(InTest), Count(InCount), Family(MoveTemp(InFamily)) {}
    virtual ~FNativeFormatBenchmark() { Restore(); }
    virtual bool Update() override
    {
        if (!Fixture) { return Start(); }
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test.AddError(TEXT("Native F benchmark exceeded its 180-second stage deadline. Last command message: ") + Completion.LastMessage);
            return Finish();
        }
        auto* Panel = Editor->GetGraphPanel();
        const auto Routes = Panel->GetMetaData<FRouteCache>();
        if (Phase == 1 || Phase == 3)
        {
            if (!Completion.CompletedAt || !Routes || !Routes->IsReady()) { return false; }
            const double RoutesObservedMs = (FPlatformTime::Seconds() - StartedAt) * 1000;
            const double CommandMs = (FMath::Max(Completion.CompletedAt, DispatchEndedAt) - StartedAt) * 1000;
            Csv += FString::Printf(TEXT("%s,%d,%d,%d,%d,%s,%d,%.6f,%.6f,%.6f,%llu,%d,%d\n"), *Family, Count, Pins, Count - 1, Sample,
                Phase == 1 ? TEXT("Format") : TEXT("Repeat"), CacheEntriesBefore, DispatchMs, CommandMs, RoutesObservedMs,
                Completion.CompletedFrame - StartedFrame, Completion.Changed, Routes->GetRoutes().FallbackCount);
            if (Sample >= 0) { (Phase == 1 ? FormatTimes : RepeatTimes).Add(CommandMs); }
            Test.TestEqual(TEXT("Completed F retains every original connection"), Routes->GetRoutes().Wires.Num(), Count - 1);
            Test.TestEqual(TEXT("Completed chain has no native routing fallback"), Routes->GetRoutes().FallbackCount, 0);
            CheckContext();
            if (Phase == 1)
            {
                Test.TestTrue(TEXT("Actual cold F changes this unformatted graph"), Completion.Changed > 0);
                Formatted = SerializeTransactionValues(*Fixture->Graph);
                Test.TestTrue(TEXT("Successful format changes graph values"), Formatted != Before);
                CheckProperties();
                Test.TestEqual(TEXT("Whole F creates exactly one undo transaction"), GEditor->Trans->GetQueueLength(), AppliedQueue + 1);
                Test.TestTrue(TEXT("A real format dirties its own asset"), Package->IsDirty());
                Test.TestTrue(TEXT("Whole format undoes"), GEditor->UndoTransaction());
                Test.TestTrue(TEXT("Undo restores the exact original graph"), Before == SerializeTransactionValues(*Fixture->Graph));
                Test.TestTrue(TEXT("Whole format redoes"), GEditor->RedoTransaction());
                Test.TestTrue(TEXT("Redo restores the exact formatted graph"), Formatted == SerializeTransactionValues(*Fixture->Graph));
                if (Test.HasAnyErrors()) { return Finish(); }
                Phase = 2; Frames = 0; Deadline = FPlatformTime::Seconds() + 180; return false;
            }
            Test.TestEqual(TEXT("Completed repeated F reports no changed nodes"), Completion.Changed, 0);
            Test.TestTrue(TEXT("Completed repeated F preserves exact graph values"), Formatted == SerializeTransactionValues(*Fixture->Graph));
            Test.TestEqual(TEXT("Completed repeated F adds no undo entry"), GEditor->Trans->GetQueueLength(), NoOpQueue);
            Test.TestEqual(TEXT("Completed repeated F preserves redo history"), GEditor->Trans->GetUndoCount(), NoOpUndo);
            Test.TestFalse(TEXT("Completed repeated F leaves the clean asset clean"), Package->IsDirty());
            if (Test.HasAnyErrors()) { return Finish(); }
            if (++Sample >= SamplesWanted) { Summarize(); Capture(); return Finish(); }
            Test.TestTrue(TEXT("Restore the same starting layout for the next cold sample"), GEditor->UndoTransaction());
            Test.TestTrue(TEXT("Every cold sample starts from the exact same graph"), Before == SerializeTransactionValues(*Fixture->Graph));
            Phase = 0; Frames = 0; Deadline = FPlatformTime::Seconds() + 180; return false;
        }
        if (++Frames < 12 || !Routes || !Routes->IsReady()) { return false; }
        if (Phase == 0)
        {
            if (Before.IsEmpty())
            {
                Before = SerializeTransactionValues(*Fixture->Graph); Properties = DescribeNodes(*Fixture->Graph);
                Anchor = FIntPoint(Entry->NodePosX, Entry->NodePosY); Editor->GetViewLocation(View, Zoom);
            }
            if (const auto Cache = Panel->GetMetaData<FMeasurementCache>()) { Cache->Invalidate(); }
            AppliedQueue = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
            BeginRequest(false); Phase = 1; return false;
        }
        NoOpQueue = GEditor->Trans->GetQueueLength(); NoOpUndo = GEditor->Trans->GetUndoCount();
        BeginRequest(true); Phase = 3; return false;
    }
private:
    bool Start()
    {
        auto* Settings = GetMutableDefault<UGlooPrintSettings>(); OriginalSettings = Settings->GetLayoutSettings();
        OriginalStyle = Settings->WireStyle; bOriginalEnabled = Settings->bFormattingEnabled; bRestore = true;
        auto& Slate = FSlateApplication::Get(); OriginalCursor = Slate.GetCursorPos();
        Settings->bFormattingEnabled = true; Settings->WireStyle = EGlooPrintWireStyle::Rounded90;
        Settings->HorizontalSpacing = 96; Settings->VerticalSpacing = 48; Settings->CommentPadding = 32; Settings->NotifyChanged();
        FParse::Value(FCommandLine::Get(), TEXT("GlooPrintBenchmarkSamples="), SamplesWanted); SamplesWanted = FMath::Clamp(SamplesWanted, 1, 100);
        Directory = FPaths::ProjectSavedDir() / TEXT("GlooPrintBenchmarks"); IFileManager::Get().MakeDirectory(*Directory, true);
        Package.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/GlooPrintNativeF_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
        Fixture = MakeUnique<FFixture>(UObject::StaticClass(), false, Package.Get());
        UK2Node_ExecutionSequence* Previous = nullptr;
        const int32 Outputs = Family == TEXT("PinHeavy") ? 64 : 2;
        for (int32 I = 0; I < Count; ++I)
        {
            auto* Node = Fixture->Add<UK2Node_ExecutionSequence>({float(((I * 7) % 10) * 512), float((I / 10) * (Outputs * 32 + 160))});
            Node->NodeGuid = FGuid(0, 0, 0, I + 1);
            for (int32 P = 2; P < Outputs; ++P) { Node->AddInputPin(); }
            if (Previous && !Fixture->Graph->GetSchema()->TryCreateConnection(Previous->GetThenPinGivenIndex(0), Node->FindPinChecked(UEdGraphSchema_K2::PN_Execute)))
            {
                Test.AddError(TEXT("Native schema refused the benchmark chain.")); return Finish();
            }
            if (!Entry) { Entry = Node; }
            for (UEdGraphPin* Pin : Node->Pins) { FString Tooltip; Node->GetPinHoverText(*Pin, Tooltip); }
            Pins += Node->Pins.Num(); Previous = Node;
        }
        Editor = SNew(SGraphEditor).GraphToEdit(Fixture->Graph).IsEditable(true);
        Window = SNew(SWindow).Title(FText::FromString(FString::Printf(TEXT("GlooPrint native F: %d %s"), Count, *Family)))
            .ClientSize(FVector2f(1450, 1000))[Editor.ToSharedRef()];
        Slate.AddWindow(Window.ToSharedRef()); Editor->SetNodeSelection(Entry, true); Editor->ZoomToFit(false); Slate.SetCursorPos(FVector2D::ZeroVector);
        GLog->AddOutputDevice(&Completion); bListening = true; Deadline = FPlatformTime::Seconds() + 180; return false;
    }
    void BeginRequest(bool bRepeat)
    {
        auto* Panel = Editor->GetGraphPanel(); const auto Cache = Panel->GetMetaData<FMeasurementCache>();
        CacheEntriesBefore = Cache ? Cache->GetEntryCount() : 0;
        if (!bRepeat) { Test.TestEqual(TEXT("Measured format begins with cold native measurement cache"), CacheEntriesBefore, 0); }
        Package->SetDirtyFlag(false); FSlateApplication::Get().SetKeyboardFocus(Panel->AsShared(), EFocusCause::SetDirectly);
        Completion.Arm(FString::Printf(TEXT("%d.%s.%d.%s"), Count, *Family, Sample, bRepeat ? TEXT("Repeat") : TEXT("Format")));
        TRACE_BOOKMARK(TEXT("GlooPrintNativeF start %s cache=%d"), *Completion.Label, CacheEntriesBefore);
        StartedFrame = GFrameCounter; StartedAt = FPlatformTime::Seconds();
        bool bHandled = false;
        {
            TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_BenchmarkNativeFKey);
            bHandled = FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
        }
        DispatchEndedAt = FPlatformTime::Seconds(); DispatchMs = (DispatchEndedAt - StartedAt) * 1000;
        Test.TestTrue(TEXT("Real native F key is handled"), bHandled);
        FSlateApplication::Get().ProcessKeyUpEvent(FKeyEvent(EKeys::F, FModifierKeysState(), 0, false, 0, 0));
        Deadline = FPlatformTime::Seconds() + 180;
    }
    void CheckContext()
    {
        Test.TestEqual(TEXT("Whole F keeps its selected anchor fixed"), FIntPoint(Entry->NodePosX, Entry->NodePosY), Anchor);
        Test.TestTrue(TEXT("Whole F retains the original selection"), Editor->GetSelectedNodes().Num() == 1 && Editor->GetSelectedNodes().Contains(Entry));
        FVector2f CurrentView; float CurrentZoom = 0; Editor->GetViewLocation(CurrentView, CurrentZoom);
        Test.TestTrue(TEXT("Whole F retains camera position and zoom"), CurrentView == View && CurrentZoom == Zoom);
    }
    void CheckProperties()
    {
        const auto After = DescribeNodes(*Fixture->Graph);
        Test.TestEqual(TEXT("Formatting retains every native property and pin field"), After.Num(), Properties.Num());
        for (const auto& Pair : Properties)
        {
            if (Pair.Key.EndsWith(TEXT(".NodePosX")) || Pair.Key.EndsWith(TEXT(".NodePosY"))) { continue; }
            const FString* Value = After.Find(Pair.Key);
            if (!Value || *Value != Pair.Value) { Test.AddError(TEXT("Native F changed non-position property: ") + Pair.Key); break; }
        }
    }
    void Summarize()
    {
        for (auto* Times : {&FormatTimes, &RepeatTimes})
        {
            Times->Sort(); const bool bFormat = Times == &FormatTimes;
            if (SamplesWanted >= 20)
            {
                Test.AddInfo(FString::Printf(TEXT("Native %d %s %s: %d samples after one warmup; successful-result p95 %.3fms, range %.3f–%.3fms. Includes native capture/planning/apply and intervening editor frames; excludes later automatic route readiness."),
                    Count, *Family, bFormat ? TEXT("cold F") : TEXT("repeated F"), SamplesWanted, (*Times)[FMath::CeilToInt(Times->Num() * 0.95) - 1], (*Times)[0], Times->Last()));
            }
            else
            {
                Test.AddInfo(FString::Printf(TEXT("Native %d %s %s diagnostic: %d samples after one warmup; successful-result range %.3f–%.3fms. No p95 or responsiveness claim."),
                    Count, *Family, bFormat ? TEXT("cold F") : TEXT("repeated F"), SamplesWanted, (*Times)[0], Times->Last()));
            }
        }
    }
    void Capture()
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (!Test.TestTrue(TEXT("Capture actual completed native F graph"), FSlateApplication::Get().TakeScreenshot(Editor.ToSharedRef(), Pixels, Size))) { return; }
        TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
        Test.TestTrue(TEXT("Save native F benchmark capture"), FFileHelper::SaveArrayToFile(Png, *(Directory / (Stem() + TEXT(".png")))));
    }
    FString Stem() const { return FString::Printf(TEXT("%d-NativeFormat-%s"), Count, *Family); }
    bool Finish()
    {
        if (!Directory.IsEmpty()) { Test.TestTrue(TEXT("Save all native F samples"), FFileHelper::SaveStringToFile(Csv, *(Directory / (Stem() + TEXT(".csv"))))); }
        Restore(); return true;
    }
    void Restore()
    {
        if (bListening) { Completion.bArmed = false; GLog->RemoveOutputDevice(&Completion); bListening = false; }
        if (Window) { Window->RequestDestroyWindow(); Window.Reset(); Editor.Reset(); }
        if (Package) { Package->SetDirtyFlag(false); }
        if (bRestore)
        {
            bRestore = false; auto* Settings = GetMutableDefault<UGlooPrintSettings>(); Settings->WireStyle = OriginalStyle;
            Settings->bFormattingEnabled = bOriginalEnabled; Settings->HorizontalSpacing = OriginalSettings.HorizontalSpacing;
            Settings->VerticalSpacing = OriginalSettings.VerticalSpacing; Settings->CommentPadding = OriginalSettings.CommentPadding; Settings->NotifyChanged();
            FSlateApplication::Get().SetCursorPos(OriginalCursor);
        }
    }
    FAutomationTestBase& Test;
    int32 Count;
    FString Family, Directory;
    FFormatCompletion Completion;
    TStrongObjectPtr<UPackage> Package;
    TUniquePtr<FFixture> Fixture;
    UK2Node_ExecutionSequence* Entry = nullptr;
    TSharedPtr<SGraphEditor> Editor;
    TSharedPtr<SWindow> Window;
    FLayoutSettings OriginalSettings;
    EGlooPrintWireStyle OriginalStyle = EGlooPrintWireStyle::Rounded90;
    TArray<uint8> Before, Formatted;
    TMap<FString, FString> Properties;
    TArray<double> FormatTimes, RepeatTimes;
    FVector2D OriginalCursor;
    FVector2f View;
    FIntPoint Anchor;
    float Zoom = 0;
    double Deadline = 0, StartedAt = 0, DispatchEndedAt = 0, DispatchMs = 0;
    uint64 StartedFrame = 0;
    int32 Phase = 0, Frames = 0, Sample = -1, SamplesWanted = 3, Pins = 0, AppliedQueue = 0, NoOpQueue = 0, NoOpUndo = 0, CacheEntriesBefore = 0;
    bool bRestore = false, bOriginalEnabled = true, bListening = false;
    FString Csv = TEXT("family,nodes,pins,links,sample,operation,measurement_entries_before,initial_dispatch_ms,command_result_ms,routes_observed_ready_ms,command_frames,changed_nodes,fallbacks\n");
};
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FNativeFormatPerformanceTest, "GlooPrint.Performance.NativeFormat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)
void FNativeFormatPerformanceTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (int32 Count : {100, 1000}) for (const TCHAR* Family : {TEXT("Ordinary"), TEXT("PinHeavy")})
    {
        const FString Name = FString::Printf(TEXT("%d.%s"), Count, Family); Names.Add(Name); Commands.Add(Name);
    }
}
bool FNativeFormatPerformanceTest::RunTest(const FString& Parameters)
{
    FString Size, Family;
    if (!Parameters.Split(TEXT("."), &Size, &Family)) { AddError(TEXT("Expected native fixture size and family.")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(FNativeFormatBenchmark(*this, FCString::Atoi(*Size), Family)); return true;
}
}
#endif

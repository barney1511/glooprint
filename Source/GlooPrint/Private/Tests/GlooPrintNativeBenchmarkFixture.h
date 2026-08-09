#pragma once
#if WITH_DEV_AUTOMATION_TESTS
#include "GlooPrintTestUtils.h"

namespace GlooPrint::Tests
{
inline bool PopulateNativeChain(FAutomationTestBase& Test, FFixture& Fixture, int32 Count, int32 Outputs,
    UK2Node_ExecutionSequence*& Entry, int32& Pins)
{
    UK2Node_ExecutionSequence* Previous = nullptr;
    for (int32 I = 0; I < Count; ++I)
    {
        auto* Node = Fixture.Add<UK2Node_ExecutionSequence>({float(((I * 7) % 10) * 512), float((I / 10) * (Outputs * 32 + 160))});
        Node->NodeGuid = FGuid(0, 0, 0, I + 1);
        for (int32 P = 2; P < Outputs; ++P) { Node->AddInputPin(); }
        if (Previous && !Fixture.Graph->GetSchema()->TryCreateConnection(Previous->GetThenPinGivenIndex(0), Node->FindPinChecked(UEdGraphSchema_K2::PN_Execute)))
        {
            Test.AddError(TEXT("Native schema refused the benchmark chain.")); return false;
        }
        if (!Entry) { Entry = Node; }
        for (UEdGraphPin* Pin : Node->Pins) { FString Tooltip; Node->GetPinHoverText(*Pin, Tooltip); }
        Pins += Node->Pins.Num(); Previous = Node;
    }
    return true;
}
}
#endif

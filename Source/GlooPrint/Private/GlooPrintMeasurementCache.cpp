#include "GlooPrintMeasurementCache.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "GraphEditAction.h"
#include "Internationalization/TextLocalizationManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/ObjectWriter.h"
#include "Styling/AppStyle.h"

namespace GlooPrint
{
FMeasurementCache::~FMeasurementCache()
{
    if (UEdGraph* LiveGraph = Graph.Get()) { LiveGraph->RemoveOnGraphChangedHandler(GraphChangedHandle); }
}

void FMeasurementCache::Begin(UEdGraph* InGraph, float LayoutScale, SGraphEditor::EPinVisibility PinVisibility)
{
    check(IsInGameThread());
    Hits = 0; Misses = 0;
    if (Graph.Get() != InGraph)
    {
        if (UEdGraph* Old = Graph.Get()) { Old->RemoveOnGraphChangedHandler(GraphChangedHandle); }
        Graph = InGraph;
        GraphChangedHandle = InGraph->AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateSP(this, &FMeasurementCache::OnGraphChanged));
        Invalidate();
    }
    const uint16 CurrentTextRevision = FTextLocalizationManager::Get().GetTextRevision();
    const void* CurrentStyle = &FAppStyle::Get();
    if (Scale != LayoutScale || Visibility != PinVisibility || TextRevision != CurrentTextRevision || StyleIdentity != CurrentStyle)
    {
        Invalidate();
        Scale = LayoutScale; Visibility = PinVisibility; TextRevision = CurrentTextRevision; StyleIdentity = CurrentStyle;
    }
    TSet<FGuid> Present;
    Present.Reserve(InGraph->Nodes.Num());
    for (const UEdGraphNode* Node : InGraph->Nodes) { Present.Add(Node->NodeGuid); }
    for (auto It = Entries.CreateIterator(); It; ++It)
    {
        if (!It.Value().Node.IsValid() || !Present.Contains(It.Key())) { It.RemoveCurrent(); }
    }
}

void FMeasurementCache::Invalidate() { Entries.Reset(); ++Revision; }

void FMeasurementCache::OnGraphChanged(const FEdGraphEditAction& Action)
{
    if (Action.Nodes.IsEmpty()) { Invalidate(); return; }
    ++Revision;
    for (const UEdGraphNode* Node : Action.Nodes)
    {
        if (IsValid(Node)) { Entries.Remove(Node->NodeGuid); }
    }
}

const FMeasuredNode* FMeasurementCache::Find(const UEdGraphNode& Node, const TArray<uint8>& State)
{
    const FEntry* Entry = Entries.Find(Node.NodeGuid);
    if (Entry && Entry->Node.Get() == &Node && Entry->State == State)
    {
        ++Hits; return &Entry->Geometry;
    }
    ++Misses; return nullptr;
}

void FMeasurementCache::Store(const UEdGraphNode& Node, TArray<uint8> State, const FMeasuredNode& Measurement)
{
    FEntry& Entry = Entries.FindOrAdd(Node.NodeGuid);
    Entry.Node = const_cast<UEdGraphNode*>(&Node);
    Entry.State = MoveTemp(State); Entry.Geometry = Measurement;
}

TArray<uint8> CaptureMeasurementState(UEdGraphNode& Node)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(GlooPrint_MeasurementState);
    TArray<uint8> State;
    FObjectWriter Writer(&Node, State, false, false, false, PPF_DuplicateVerbatim);
    FString Title = Node.GetNodeTitle(ENodeTitleType::FullTitle).ToString();
    bool bHasMessage = Node.bHasCompilerMessage;
    Writer << Title << bHasMessage << Node.ErrorType << Node.ErrorMsg;
    for (const UEdGraphPin* Pin : Node.Pins)
    {
        uint64 Identity = reinterpret_cast<UPTRINT>(Pin);
        Writer << Identity;
    }
    return State;
}
}

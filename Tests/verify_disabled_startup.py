"""Read an existing saved fixture in a disposable editor with GlooPrint absent.

Run through UE 5.8's ExecutePythonScript, with fixture.json in the host project.
No asset is saved. Desktop gestures and full serialized identity checks are
separate from this public-API startup/load/compile/edit check.
"""
import json
from pathlib import Path
import traceback

import unreal


directory = Path(unreal.Paths.project_dir()).resolve()
metadata = json.loads((directory / "fixture.json").read_text())
result = {"stage": "starting", "checks": []}
blueprints = unreal.BlueprintEditorLibrary
pins = unreal.BlueprintGraphPinLibrary


def record():
    (directory / "verification.json").write_text(json.dumps(result, indent=2) + "\n")


def graph_state():
    nodes, links = {}, []
    # EdGraph.Nodes is protected in Python. The public object iterator exposes
    # the loaded native node objects without bypassing that property boundary.
    for node in unreal.ObjectIterator(unreal.EdGraphNode):
        if node.get_outer() != graph:
            continue
        name = node.get_name()
        nodes[name] = {"class": node.get_class().get_path_name()}
        if isinstance(node, unreal.K2Node):
            position = blueprints.get_node_pos(node)
            nodes[name]["position"] = [position.x, position.y]
            for pin in blueprints.list_output_pins(node):
                for target in pins.list_connected_pins(pin):
                    links.append([name, str(pins.get_pin_name(pin)),
                                  pins.get_owning_node(target).get_name(), str(pins.get_pin_name(target))])
        else:
            assert isinstance(node, unreal.EdGraphNode_Comment)
            nodes[name]["comment"] = blueprints.get_comment_text(node)
    return {"nodes": nodes, "links": sorted(links)}


try:
    # Disabled in the project, descriptor-only installation: no plugin binary
    # can supply asset classes or editor behavior to this process.
    project = json.loads(next(directory.glob("*.uproject")).read_text())
    assert any(p["Name"] == "GlooPrint" and p["Enabled"] is False for p in project["Plugins"])
    assert not (directory / "Plugins/GlooPrint/Binaries").exists()
    assert not hasattr(unreal, "GlooPrintSettings")
    blueprint = unreal.load_asset(metadata["package"])
    assert blueprint is not None, "Saved Blueprint did not load"
    graph = blueprints.find_graph(blueprint, metadata["graph"])
    assert graph is not None, "Saved graph did not load"
    # Public scripting does not expose raw node/pin GUIDs or every serialized
    # property. Those remain covered by the earlier C++ disk round-trip case.
    # This check adds true disabled-startup evidence without private access.
    expected_nodes = {}
    for saved in metadata["nodes"].values():
        value = {"class": saved["class"]}
        if saved["class"].endswith("EdGraphNode_Comment"):
            value["comment"] = saved["comment"]
        else:
            value["position"] = saved["bounds"][:2]
        expected_nodes[saved["name"]] = value
    expected_links = sorted([[metadata["nodes"][a]["name"], b, metadata["nodes"][c]["name"], d]
                             for a, b, c, d in metadata["links"]])
    expected = {"nodes": expected_nodes, "links": expected_links}
    before = graph_state()
    result["loaded_state"] = before
    assert before == expected, "Loaded node names/classes/positions/comments/connections differ from the saved baseline"
    result["checks"].append("saved node names/classes, K2 positions, comment text and all seven connections")
    assert blueprints.compile_blueprint(blueprint), "Native Blueprint compilation failed"
    assert graph_state() == before, "Compilation changed the saved graph"
    result["checks"].append("native compile preserves the saved graph")
    node = unreal.find_object(graph, "K2Node_CustomEvent_0", unreal.K2Node)
    assert node is not None
    position = blueprints.get_node_pos(node)
    with unreal.ScopedEditorTransaction("Move node with GlooPrint absent"):
        blueprints.set_node_pos(node, unreal.IntPoint(position.x + 128, position.y + 64))
    moved = graph_state()
    expected_moved = json.loads(json.dumps(before))
    expected_moved["nodes"][node.get_name()]["position"] = [position.x + 128, position.y + 64]
    assert moved == expected_moved, "Native node edit changed unexpected graph values"
    with unreal.ScopedEditorTransaction("Restore saved node position"):
        blueprints.set_node_pos(node, position)
    assert graph_state() == before, "Native restore changed saved graph values"
    assert blueprints.compile_blueprint(blueprint)
    assert graph_state() == before
    result["checks"].append("native node move and explicit restore preserve the other nodes and connections")
    assert not hasattr(unreal, "GlooPrintSettings")
    assert unreal.get_editor_subsystem(unreal.AssetEditorSubsystem).open_editor_for_assets([blueprint])
    result["checks"].append("ordinary Blueprint editor opens")
    result["stage"] = "passed"
    result["limitations"] = ["desktop wire/gesture review not completed in this run",
                             "node/pin GUIDs and complete serialized properties are not exposed by this public scripting API",
                             "position restore uses an explicit native edit, not undo/redo"]
    record()
except Exception:
    result["stage"] = "failed"
    result["error"] = traceback.format_exc()
    record()
    raise

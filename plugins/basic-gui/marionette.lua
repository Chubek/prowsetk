-- Trusted GUI marionette: edit the permitted actions for your page.
-- main(args) prepares the live global session and returns decisions JSON.
-- The host sends args.goal (when supplied in the GUI) to OpenCode, which
-- selects action IDs only. Model responses are never executed as code.
function main(args)
    assert(session:document(), "Load a page first")
    return [[{
        "version": 1,
        "goal": "Explore the permitted page controls and discover API endpoints",
        "max_steps": 8,
        "max_page_requests": 64,
        "max_get_probes": 0,
        "actions": [
            {"id": "next-page", "kind": "click", "selector": "a[rel=next]", "max_uses": 8}
        ]
    }]]
end

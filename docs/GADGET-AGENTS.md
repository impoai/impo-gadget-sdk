# Gadget agents

Design for the layer between the Impo agent and the gadgets: small agents a
person makes in plain language, bound to their gadgets, that run on what
happens in the physical world. Status: proposal, nothing built yet.

## Why

Today a gadget can do two things: answer the Impo agent's commands, and send
an event (`shaken`, `picked_up`) that lands in the person's main chat. That is
enough for "take a photo" and for a remark when the robot is picked up. It is
not enough for the things people actually want once they own one:

- "When I pick it up in the morning, say good morning and read me today's
  first meeting."
- "Every ten minutes take a photo; if someone is in the room who wasn't
  before, tell me on my phone."
- "If it gets shaken, play the dog-bark sound."
- "Drive to the door when the doorbell rings" (once the base works).

Each of these is a trigger, a few actions, a little state, and some judgement.
None of them should go through the main chat every time: that is slow (an
agent turn is ten seconds), costly, and it clutters the conversation. They
are what Dreamer calls agents built by the sidekick: user-space programs that
run on triggers, with their own data, hosted by the platform, mediated by the
kernel. Dreamer had no physical world; this is that model extended to one.

## The rings

```
ring 0  kernel     the Impo agent, the gateway, the API
                   permissions, routing, memory, model choice, rate limits
ring 1  gadget     user-made automations bound to gadgets        ← this document
        agents     triggers → actions, with state, hosted
ring 2  devices    firmware; the capability vocabulary
                   exported interface = commands + capabilities + events
ring 3  ecosystem  skills/: other people's devices, through a gadget's tunnel
```

Rule from Dreamer, kept whole: **ring 1 never touches a device directly.** A
gadget agent asks the kernel to run a command, and the kernel decides: is the
command in this agent's allowlist, is the gadget online, is the speaker free,
has this agent spent its budget. One place for policy; the firmware and the
gateway stay simple.

## What a gadget agent is

```jsonc
{
  "id": "ga_8f3…",
  "name": "Morning greeting",
  "owner": "user_…",
  "gadgets": ["homelink-6949c4"],            // the gadgets it may use
  "triggers": [
    { "kind": "gadget.event", "gadget": "homelink-6949c4", "event": "picked_up",
      "when": { "hours": [6, 10] } },         // optional filters: hours, days, data fields
    { "kind": "schedule", "cron": "0 8 * * 1-5", "timeZone": "America/Los_Angeles" },
    { "kind": "message", "mention": "@greeter" },        // from the app or a gadget's chat
    { "kind": "webhook", "secret": "…" }                 // later
  ],
  "allow": {
    "commands": ["speaker.say", "speaker.play_url", "display.show_text", "avatar.cheer"],
    "tools": ["calendar.today", "weather"],   // platform tools, by name
    "physical": false                         // chassis.*, anything that moves: off unless granted
  },
  "budget": { "runsPerHour": 12, "cooldownSeconds": 60, "tokensPerRun": 4000 },
  "instructions": "When I pick it up in the morning, say good morning by name and read the first calendar event. Keep it to two sentences.",
  "kind": "llm",                              // or "rules"
  "state": {},                                // a small JSON document the agent may read and write
  "enabled": true
}
```

Two kinds, chosen by the kernel when it builds the agent:

- **rules**: no model at run time. Trigger → fixed actions with parameter
  templates (`speaker.play_url {url: "…/bark.mp3"}`). Runs in milliseconds,
  costs nothing, never surprises. The kernel compiles obvious requests into
  this kind ("when shaken, play X").
- **llm**: a prompt (the instructions, the person's profile and memory, the
  trigger's data, the agent's state) with exactly the allowed commands and
  tools, run as a short bounded session on the worker. For anything with
  judgement in it.

An agent exports its interface to the kernel the way a gadget does: its
triggers, what it may do, and a `run(trigger)` entry. The main agent can call
it as a tool too ("run my morning greeting now").

## Runtime

```
gateway ──link.event──▶ API  POST /gadgets/events  (service token, account subject)
                              │
                              ▼
                        event router
                        ├─ gadget agents whose triggers match → run each (parallel, budgeted)
                        └─ none matched → main chat, as today ("[Gadget event] …")

scheduler (Temporal, already there) ──schedule trigger──▶ run
app message "@greeter …" ──message trigger──▶ run
```

A run:

1. The router builds the trigger payload (`event`, `data`, `gadget`,
   `at`, agent `state`).
2. `rules`: the worker executes the actions in order through the kernel's
   gadget invoke path (the same `gadgets.invoke` the main agent uses), with
   the agent's allowlist and budget applied.
3. `llm`: the worker opens a bounded session (a few tool calls, the token
   budget), with tools limited to `allow`; every gadget command still goes
   through the kernel path. The reply, if any, is spoken on the gadget
   (`speaker.say`) or sent to the phone (a notification), as the agent's
   instructions say.
4. The run is logged (`gadget_agent_runs`: trigger, actions, result, cost,
   duration) and `state` is written back.

Arbitration in the kernel path, because several things may want the gadget
at once: a person's own voice turn wins over any agent; one sound at a time
on a speaker (a later `say` waits, then plays, or is dropped if older than
30 s); physical commands need `allow.physical`; quiet hours per gadget.

## Making one

Nothing new for the person to learn: they tell the main agent what they
want. The main agent is the sidekick in coding mode:

1. **Plan**: it reads the person's gadgets (`impo_list_gadgets`: capabilities
   and commands) and the platform tools, and says what it will build, or
   that it can't (no camera, no base).
2. **Build**: it calls `impo_create_gadget_agent` with the spec above. The
   API validates it: gadgets belong to the account, commands exist on them,
   tools exist, budgets within limits, no `physical` without the person
   saying so.
3. **Test**: the API runs it once with a synthetic trigger (`dry_run: true`:
   actions are checked and reported, not sent), then once for real if the
   person wants ("try it now").
4. **Keep**: it appears in the app under Settings → Gadgets → the gadget's
   automations, with a switch, the last run and its log. The person can also
   ask the main agent to change or remove it.

Later, the same spec can be edited as a file through the CLI, as Dreamer
does: the agent and the engineer use one interface.

## Data

- `gadget_agents`: id, user, name, spec (JSON), kind, enabled, created,
  updated.
- `gadget_agent_runs`: agent, trigger, started, finished, status, actions
  (JSON), error, tokens.
- State lives in the spec row (a small JSON document, 64 KB cap); an agent
  that needs more gets the per-agent store Dreamer describes (a SQLite file
  per agent, rows owned per user) in a later phase.

## Protocol

Nothing changes on the gadget for the first phase: `link.event` exists,
commands exist. The gateway stops posting events as chat messages itself and
posts them to `POST /api/v1/gadgets/events`, which routes. Later phases add:

- `camera.watch {fps, seconds}`: an observation stream. The gadget pushes
  frames on a stream; a perception worker (a cheap vision model plus change
  detection) turns them into events (`scene_changed`, `person_seen`) that
  trigger gadget agents like any other event. The main agent is never in
  the frame loop.
- `link.event` names from the vocabulary get filters in triggers
  (`data.strength == "hard"`).

## Phases

1. **Rules only.** Event router, `gadget_agents` table, `rules` kind, the
   create/update/delete/list API, the main agent's tools, the app list with
   a switch. Covers "when shaken play X", "when picked up say Y", schedules
   that say or show something. A week of work across API, worker, app.
2. **LLM agents.** Bounded sessions with allowlisted tools; the arbitration
   rules; run logs in the app. "Read me my first meeting when picked up."
3. **Observation.** `camera.watch`, the perception worker, scene events.
   "Tell me when someone comes in."
4. **Sharing.** A gallery of gadget agents by board (the Dreamer gallery for
   hardware): "install the Desk Companion pack for the SparkBot".

## What this is not

- Not a scripting language on the gadget. The firmware stays dumb and
  reliable; the agents live on the platform where the models, the memory
  and the person's accounts are.
- Not a replacement for the main agent. The main agent is still where the
  person talks; gadget agents are what runs when the person isn't talking.
- Not a way around the capability contract. An agent can only use what a
  gadget registered, and only what the person allowed.






Add to M10:
how to decrease mid-word delays?
With high WPM tempo words became quick, but delays between words not so small, and speech
feels fragmentary/chopped.
What parameters does TTS already have? I want to see all of them.
What if we synthesize not 200 WPM but 160 and then speedup playback?




Add to M8A:
create multiple system prompts
- AI expert consultant
- obsessed geeky AI enthusiast
- ingenue, frivolous, absent-minded, playful and cheerful
- aggressive milatary nazi
- ideomatic kommunist
- abstract phylosopher and writer - materialist
- abstract phylosopher and writer - idealist
- robot Bender Rodrigez (from Futurama)
What other existing settings may work well together with sysprompt defining personality?
Let's combine them together in config and make registry of personalities.





--------------------------------------------------------------------------------


convert plans and docs into single-file design with history of acva creation
process it and load into memory + add memory read instructions into context
so agent can answer questions about itself




Can we split internal explicit LLM thinking and speaking?
Optionally enable verbal thinking



Goals:
    - ready to use and higly customizable
    - modular experimental platform
    - local-first (privacy, connection)
    - Voice UX


Modular:
    - after obvious modules - separate services (tts, stt, llm), what else?
        - audio processing
        - memory
        - tools
        - what else?
            can FSM be a module for example?
            can AEC be a module?
            can pattern builder be a module?
            what are pros/cons separating component to a module?
    - orchestrator loads configurable runtime plugins - modules implementation
    - do not mixup modularity and configurability


Sound filters cheep way to make voice more natural

SSML

artistic speaking with expression

sarcastic

compare:
    - min default config: no humanization, plain direct question-answer, default computer voice
    - max human-like config: voice, drift, background thinking, etc...


Followups:
- tools integration
    - first tool - own config modification and restart
    - knowledge graph
    - diagrams and presentations
    - quiz
    - web search
- web ui - plans/web_ui_architecture.md
- docs, readme for advanced users plus arch overview, internals in docs
- MacOS Metal
- Windows

Other potential improvements ???

all-in-one C++ app (llama.cpp + speeches, libs instead of servers) ???

?? Interleaving with players (music, browser, etc)

multi-user conversation
    - detect users by voice and remember
    - remember main user by voice

generate copy of user personality

multi-context with single model
multi-model runtime


multi-user conversation -> interleaved STT - solution???



game NPC character design
    - user switch talking between character, admin, other characters
    - ask characters talk to each other


different database, vector database, graph database

llama.cpp vs ollama

speeches systemd service ???


reasoning
temperature
person state from memory to promt
style post processing
drift policy
2-stage generation




Installations:
- docker
- native packages - systemd service with deps, deb, rpm, ...
- native all-in-one - snap/flatpack/appimage with/wo servers
- ??? single all-in-one executable, run as single process

UX:
- background service + tray icon with quick small menu + web-ui
- desktop app with electron-based UI, loads server web-ui ???



CI/CD, release

gh pages


Compare with other tools
analyze target audiences, usage patterns, required functions
can:
    - user - Voice UX only
        - offline Siri/Alexa, desktop Linux
        - try new open/free models
    - advanced user - edit config via UI
        - ollama is a docker for LLMs
        - speeches is ollama for STT/TTS
        - 
    - context engineering - read session, history, dialogs, memory; edit
    - agent engineering - tools, memories, integrations
    - runtime engineering - sandbox, FSM, pipeline, modules
    - what else?


tg on-call assistent

low-latency online voice translation


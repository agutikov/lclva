




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
- native all in one - snap/flatpack/appimage with/wo servers


CI/CD, release

gh pages


Compare with other tools
analyze target audiences, usage patterns, required functions


tg on-call assistent

low-latency online voice translation




ananlyze last log

I hear counting from 1 only to 34, something stops tts or playback
Self-listening if even worse - it transcripts only to 13

I don't want any limits for playback until barge-in implementation;
If I ask llm to tell a long long story I then want to listen to full monologe till the end
or till the LLM stops;
Do not interrupt audio, let user listen to the whole LLM output

When will implement barg-in - then will decide what to do with the rest of not played words


what left for M6?



filter llm output because it may hallucinate and mix languages in answer
leave only selected languge


Can we split internal explicit thinking and speaking?


--------------------------------------------------------------------------------


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


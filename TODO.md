



add cmdline memory crud tool in M8



M8? fast restart if stuck or to apply params or request - save context with timestamp, restart, load context and continue



BUG: VAD skips first letter-to-word, why? How to fix? Why it doesn't use sliding window?


BUG: Long text speech cuts at some lenght threshold, LLM is not aware of it


--------------------------------------------------------------------------------


Goals:
    - ready to use and higly customizable
    - modular experimental platform
    - local-first
    - Voice UX


Sound filters cheep way to make voice more natural

SSML

artistic speaking with expression

sarcastic


Followups:
- tools integration
- web ui - plans/web_ui_architecture.md
- docs, readme for advanced users plus arch overview, internals in docs
- MacOS Metal
- Windows

Other potential improvements ???

all-in-one C++ app (llama.cpp + speeches, libs instead of servers) ???

?? Interleaving with players (music, browser, etc)

multi-user conversation
    - detect users by voice and remember

generate copy of user personality

multi-context with single model
multi-model runtime


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


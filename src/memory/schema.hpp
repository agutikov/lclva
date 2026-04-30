#pragma once

namespace lclva::memory {

// Schema v1. Mirrors project_design.md §9.1. Idempotent: every CREATE uses
// IF NOT EXISTS; every CREATE INDEX is also IF NOT EXISTS.
//
// Bumping this requires a migration plan (deferred until we have shipped
// schema state worth migrating from).
inline constexpr const char* kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS sessions (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    started_at  INTEGER NOT NULL,
    ended_at    INTEGER,
    title       TEXT
);

CREATE TABLE IF NOT EXISTS turns (
    id                       INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id               INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    role                     TEXT NOT NULL CHECK (role IN ('user','assistant')),
    text                     TEXT,
    lang                     TEXT,
    started_at               INTEGER NOT NULL,
    ended_at                 INTEGER,
    status                   TEXT NOT NULL CHECK (status IN ('in_progress','committed','interrupted','discarded')),
    interrupted_at_sentence  INTEGER,
    audio_path               TEXT
);

CREATE TABLE IF NOT EXISTS summaries (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id        INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    range_start_turn  INTEGER NOT NULL,
    range_end_turn    INTEGER NOT NULL,
    summary           TEXT NOT NULL,
    lang              TEXT NOT NULL,
    source_hash       TEXT NOT NULL,
    created_at        INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS facts (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    key             TEXT NOT NULL,
    value           TEXT NOT NULL,
    lang            TEXT,
    source_turn_id  INTEGER REFERENCES turns(id) ON DELETE SET NULL,
    confidence      REAL NOT NULL,
    updated_at      INTEGER NOT NULL,
    UNIQUE(key, lang)
);

CREATE TABLE IF NOT EXISTS settings (
    key         TEXT PRIMARY KEY,
    value       TEXT NOT NULL,
    updated_at  INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_turns_session     ON turns(session_id, id);
CREATE INDEX IF NOT EXISTS idx_summaries_session ON summaries(session_id, range_end_turn);
CREATE INDEX IF NOT EXISTS idx_facts_key         ON facts(key);
)sql";

} // namespace lclva::memory

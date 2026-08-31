-- =============================================================
-- Row Level Security — paste this into the SQL Editor
-- Enables anon SELECT (so Realtime reaches the browser)
-- and grants service_role full access (server-side writes).
-- Safe to run: all statements are idempotent (drop if exists).
-- =============================================================

ALTER TABLE samples     ENABLE ROW LEVEL SECURITY;
ALTER TABLE alerts      ENABLE ROW LEVEL SECURITY;
ALTER TABLE live_state  ENABLE ROW LEVEL SECURITY;
ALTER TABLE heartbeats  ENABLE ROW LEVEL SECURITY;

-- Allow anon SELECT on samples (Realtime subscription)
DROP POLICY IF EXISTS "anon select samples" ON samples;
CREATE POLICY "anon select samples"
  ON samples FOR SELECT TO anon USING (true);

-- Allow anon SELECT on alerts (Realtime subscription)
DROP POLICY IF EXISTS "anon select alerts" ON alerts;
CREATE POLICY "anon select alerts"
  ON alerts FOR SELECT TO anon USING (true);

-- service_role: full access on all tables (bypasses RLS)
DROP POLICY IF EXISTS "service select samples" ON samples;
CREATE POLICY "service select samples" ON samples FOR SELECT TO service_role USING (true);
DROP POLICY IF EXISTS "service insert samples" ON samples;
CREATE POLICY "service insert samples" ON samples FOR INSERT TO service_role WITH CHECK (true);
DROP POLICY IF EXISTS "service delete samples" ON samples;
CREATE POLICY "service delete samples" ON samples FOR DELETE TO service_role USING (true);

DROP POLICY IF EXISTS "service select alerts" ON alerts;
CREATE POLICY "service select alerts" ON alerts FOR SELECT TO service_role USING (true);
DROP POLICY IF EXISTS "service insert alerts" ON alerts;
CREATE POLICY "service insert alerts" ON alerts FOR INSERT TO service_role WITH CHECK (true);

DROP POLICY IF EXISTS "service select live" ON live_state;
CREATE POLICY "service select live" ON live_state FOR SELECT TO service_role USING (true);
DROP POLICY IF EXISTS "service insert live" ON live_state;
CREATE POLICY "service insert live" ON live_state FOR INSERT TO service_role WITH CHECK (true);
DROP POLICY IF EXISTS "service update live" ON live_state;
CREATE POLICY "service update live" ON live_state FOR UPDATE TO service_role USING (true) WITH CHECK (true);

DROP POLICY IF EXISTS "service select hb" ON heartbeats;
CREATE POLICY "service select hb" ON heartbeats FOR SELECT TO service_role USING (true);
DROP POLICY IF EXISTS "service insert hb" ON heartbeats;
CREATE POLICY "service insert hb" ON heartbeats FOR INSERT TO service_role WITH CHECK (true);
DROP POLICY IF EXISTS "service update hb" ON heartbeats;
CREATE POLICY "service update hb" ON heartbeats FOR UPDATE TO service_role USING (true) WITH CHECK (true);

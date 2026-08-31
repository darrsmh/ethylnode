import { Redis } from "@upstash/redis";

export const redis = new Redis({
  url: process.env.UPSTASH_REDIS_REST_URL!,
  token: process.env.UPSTASH_REDIS_REST_TOKEN!,
});

const LIVE_KEY = "seismic:live";
const SAMPLES_KEY = "seismic:samples";
const ALERTS_KEY = "seismic:alerts";
const HEARTBEAT_KEY = "seismic:heartbeat";
const MAX_SAMPLES = 600;
const MAX_ALERTS = 50;

export async function updateLive(data: Record<string, unknown>) {
  await redis.hset(LIVE_KEY, data);
}

export async function getLive() {
  const live = await redis.hgetall(LIVE_KEY);
  return live && typeof live === "object" ? live : {};
}

export async function pushSamples(samples: object[]) {
  if (!samples.length) return;
  const multi = redis.multi();
  for (const s of samples) {
    multi.zadd(SAMPLES_KEY, { score: (s as any).ts, member: JSON.stringify(s) });
  }
  multi.zremrangebyrank(SAMPLES_KEY, 0, -(MAX_SAMPLES + 1));
  await multi.exec();
}

export async function getSamples(count = 200) {
  const raw = await redis.zrange(SAMPLES_KEY, 0, -1, { rev: true });
  if (!Array.isArray(raw)) return [];
  const parsed = raw
    .slice(0, count)
    .filter((r) => typeof r === "string")
    .map((r) => {
      try {
        return JSON.parse(r as string);
      } catch {
        return null;
      }
    })
    .filter((s) => s !== null);
  return parsed.reverse();
}

export async function pushAlert(alert: object) {
  await redis.lpush(ALERTS_KEY, JSON.stringify(alert));
  await redis.ltrim(ALERTS_KEY, 0, MAX_ALERTS - 1);
}

export async function getAlerts(count = 20) {
  const raw = await redis.lrange(ALERTS_KEY, 0, count - 1);
  if (!Array.isArray(raw)) return [];
  return raw
    .filter((r) => typeof r === "string")
    .map((r) => {
      try {
        return JSON.parse(r as string);
      } catch {
        return null;
      }
    })
    .filter((a) => a !== null);
}

export async function setHeartbeat(nodeId: string, data: Record<string, unknown>) {
  await redis.hset(`${HEARTBEAT_KEY}:${nodeId}`, data);
}

export async function getHeartbeat(nodeId: string) {
  return redis.hgetall(`${HEARTBEAT_KEY}:${nodeId}`);
}

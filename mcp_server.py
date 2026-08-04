#!/usr/bin/env python3
"""Ark Viewer 2 性能诊断 MCP 服务器

读取 perf_data/*.jsonl 落盘日志，暴露性能数据给 AI 诊断卡顿。
启动方式：start_mcp.bat 或 python mcp_server.py

JSONL 行格式（由 C++ 侧 ActivityLog 写入）：
  {"ts":1722470400123,"thread_id":12345,"thread_name":"Decode",
   "type":"event|snapshot|stall|activity","category":"解码",
   "op":"WicDecoder::DecodeLevel","duration_ms":18.4,
   "extra":{"file":"a.jpg","w":8000,"level":2}}

type 说明：
  event    - 普通事件（PerfScope 析构时记录，<100ms）
  stall    - 卡顿事件（PerfScope 析构时记录，>=100ms，额外含 stack 字段）
  snapshot - 周期快照（每秒由 CollectSnapshot 写入，含 mem/bitmap/tile 等）
  activity - F12 日志镜像（Log() 时镜像，供 search_logs 检索）
"""

import json
import os
import glob
import statistics
from fastmcp import FastMCP


def _find_perf_dir() -> str:
    """定位 perf_data 目录：优先环境变量，其次 build/perf_data，最后 ./perf_data"""
    env = os.environ.get("ARK_PERF_DIR")
    if env and os.path.isdir(env):
        return env
    script_dir = os.path.dirname(os.path.abspath(__file__))
    for candidate in (os.path.join(script_dir, "build", "perf_data"),
                      os.path.join(script_dir, "perf_data")):
        if os.path.isdir(candidate):
            return candidate
    # 默认 build/perf_data（set_profiling 时自动创建）
    return os.path.join(script_dir, "build", "perf_data")


PERF_DIR = _find_perf_dir()
CONTROL_FILE = os.path.join(PERF_DIR, "control")

mcp = FastMCP("ArkViewer2-Perf")


# ─── 日志读取 ───

def _read_all_logs() -> list[dict]:
    """读取 perf_data 下所有 perf_*.jsonl，返回解析后的行列表（按 ts 排序）"""
    rows: list[dict] = []
    for path in sorted(glob.glob(os.path.join(PERF_DIR, "perf_*.jsonl"))):
        try:
            with open(path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        rows.append(json.loads(line))
                    except json.JSONDecodeError:
                        continue
        except OSError:
            continue
    rows.sort(key=lambda r: r.get("ts", 0))
    return rows


def _percentile(values: list[float], p: float) -> float:
    """计算百分位数（p: 0-100）"""
    if not values:
        return 0.0
    s = sorted(values)
    k = (len(s) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(s) - 1)
    if f == c:
        return s[f]
    return s[f] + (s[c] - s[f]) * (k - f)


def _stats(values: list[float]) -> dict:
    """统计摘要：min/avg/p95/max/count"""
    if not values:
        return {"min": 0, "avg": 0, "p95": 0, "max": 0, "count": 0}
    return {
        "min": round(min(values), 2),
        "avg": round(statistics.mean(values), 2),
        "p95": round(_percentile(values, 95), 2),
        "max": round(max(values), 2),
        "count": len(values),
    }


def _ts_to_str(ts: int) -> str:
    """Unix 毫秒时间戳转可读字符串"""
    try:
        return __import__("datetime").datetime.fromtimestamp(ts / 1000).strftime("%H:%M:%S.%f")[:-3]
    except Exception:
        return str(ts)


# ─── 9 个 Tool ───

@mcp.tool()
def get_perf_summary() -> str:
    """性能总览：卡顿次数、慢操作 Top10、内存趋势、瓦片命中率。

    返回 JSON 字符串，字段：
    - stall_count: 卡顿事件数（type=stall，>=100ms）
    - event_count: 事件总数（type=event）
    - snapshot_count: 快照数（type=snapshot）
    - slow_ops_top10: 慢操作 Top10（按 duration_ms 降序）
    - mem_trend: 内存趋势（最近 20 个 snapshot）
    - tile_hit_rate: 瓦片缓存命中率（0-1）
    """
    rows = _read_all_logs()
    stalls = [r for r in rows if r.get("type") == "stall"]
    events = [r for r in rows if r.get("type") == "event"]
    snapshots = [r for r in rows if r.get("type") == "snapshot"]

    slow = sorted(rows, key=lambda r: r.get("duration_ms", 0), reverse=True)[:10]
    slow_top10 = [{
        "op": r.get("op", ""),
        "category": r.get("category", ""),
        "duration_ms": r.get("duration_ms", 0),
        "thread_name": r.get("thread_name", ""),
        "time": _ts_to_str(r.get("ts", 0)),
        "extra": r.get("extra", {}),
    } for r in slow]

    mem_trend = [{
        "time": _ts_to_str(s.get("ts", 0)),
        "mem_ws_mb": s.get("extra", {}).get("mem_ws_mb", 0),
        "bitmap_mb": s.get("extra", {}).get("bitmap_mb", 0),
    } for s in snapshots[-20:]]

    total_hit = sum(s.get("extra", {}).get("tile_hit", 0) for s in snapshots)
    total_miss = sum(s.get("extra", {}).get("tile_miss", 0) for s in snapshots)
    hit_rate = total_hit / (total_hit + total_miss) if (total_hit + total_miss) > 0 else 0

    return json.dumps({
        "stall_count": len(stalls),
        "event_count": len(events),
        "snapshot_count": len(snapshots),
        "slow_ops_top10": slow_top10,
        "mem_trend": mem_trend,
        "tile_hit_rate": round(hit_rate, 4),
        "tile_total_hit": total_hit,
        "tile_total_miss": total_miss,
    }, ensure_ascii=False, indent=2)


@mcp.tool()
def get_slow_operations(threshold_ms: float = 100.0) -> str:
    """慢操作明细，按线程分组（UI 线程=卡顿，后台线程=吞吐问题）。

    参数：
    - threshold_ms: 耗时阈值，默认 100ms

    返回 JSON，字段：
    - threshold_ms: 阈值
    - total_slow: 慢操作总数
    - by_thread: {thread_name: {count, max_ms, avg_ms, ops: [...]}}
    """
    rows = _read_all_logs()
    slow = [r for r in rows if r.get("duration_ms", 0) >= threshold_ms]

    by_thread: dict[str, dict] = {}
    for r in slow:
        tn = r.get("thread_name", "Unknown")
        if tn not in by_thread:
            by_thread[tn] = {"count": 0, "max_ms": 0, "total_ms": 0, "ops": []}
        e = by_thread[tn]
        e["count"] += 1
        e["max_ms"] = max(e["max_ms"], r.get("duration_ms", 0))
        e["total_ms"] += r.get("duration_ms", 0)
        e["ops"].append({
            "op": r.get("op", ""),
            "category": r.get("category", ""),
            "duration_ms": r.get("duration_ms", 0),
            "time": _ts_to_str(r.get("ts", 0)),
            "extra": r.get("extra", {}),
        })

    for e in by_thread.values():
        e["avg_ms"] = round(e["total_ms"] / e["count"], 2) if e["count"] > 0 else 0
        e["total_ms"] = round(e["total_ms"], 2)

    return json.dumps({
        "threshold_ms": threshold_ms,
        "total_slow": len(slow),
        "by_thread": by_thread,
    }, ensure_ascii=False, indent=2)


@mcp.tool()
def get_navigation_latency() -> str:
    """翻页两阶段延迟统计（核心指标）。

    占位帧快=感知流畅，完整帧慢=大图解码瓶颈。

    返回 JSON，字段：
    - nav_to_placeholder_ms: 占位帧延迟统计 {min, avg, p95, max, count}
    - nav_to_full_ms: 完整帧延迟统计 {min, avg, p95, max, count}
    - samples: 最近 20 次翻页记录
    """
    rows = _read_all_logs()
    placeholder = [r for r in rows if r.get("op") == "nav_to_placeholder_ms"]
    full = [r for r in rows if r.get("op") == "nav_to_full_ms"]

    placeholder_stats = _stats([r.get("duration_ms", 0) for r in placeholder])
    full_stats = _stats([r.get("duration_ms", 0) for r in full])

    # 最近 20 次翻页：placeholder 配对同 idx 的 full
    recent = placeholder[-20:]
    samples = []
    for p in recent:
        p_ts = p.get("ts", 0)
        p_idx = p.get("extra", {}).get("idx")
        matched = None
        for f in full:
            if f.get("ts", 0) >= p_ts and f.get("extra", {}).get("idx") == p_idx:
                matched = f
                break
        samples.append({
            "time": _ts_to_str(p_ts),
            "file": p.get("extra", {}).get("file", ""),
            "idx": p_idx,
            "placeholder_ms": p.get("duration_ms", 0),
            "full_ms": matched.get("duration_ms", 0) if matched else None,
        })

    return json.dumps({
        "nav_to_placeholder_ms": placeholder_stats,
        "nav_to_full_ms": full_stats,
        "samples": samples,
    }, ensure_ascii=False, indent=2)


@mcp.tool()
def get_render_stats() -> str:
    """RenderFrame 耗时、帧间隔序列。

    返回 JSON，字段：
    - stats: {min, avg, p95, max, count}
    - frame_intervals_ms: 最近 50 帧间隔（ms）
    - recent_durations_ms: 最近 50 次 RenderFrame 耗时
    """
    rows = _read_all_logs()
    renders = [r for r in rows if r.get("op") == "RenderFrame"]
    vals = [r.get("duration_ms", 0) for r in renders]

    recent = renders[-50:]
    intervals = []
    for i in range(1, len(recent)):
        intervals.append(recent[i].get("ts", 0) - recent[i - 1].get("ts", 0))

    return json.dumps({
        "stats": _stats(vals),
        "frame_intervals_ms": intervals,
        "recent_durations_ms": [r.get("duration_ms", 0) for r in recent],
    }, ensure_ascii=False, indent=2)


@mcp.tool()
def get_tile_stats() -> str:
    """瓦片命中率、队列深度、瓦片解码耗时。

    返回 JSON，字段：
    - hit_rate_summary: {total_hit, total_miss, hit_rate}
    - queue_depth_trend: 最近 20 次活跃工作线程数
    - decode_time_by_decoder: 各解码器瓦片解码耗时 {decoder: {count, avg_ms, max_ms}}
    - active_workers_trend: 活跃工作线程数趋势
    """
    rows = _read_all_logs()
    snapshots = [r for r in rows if r.get("type") == "snapshot"]

    total_hit = sum(s.get("extra", {}).get("tile_hit", 0) for s in snapshots)
    total_miss = sum(s.get("extra", {}).get("tile_miss", 0) for s in snapshots)
    hit_rate = total_hit / (total_hit + total_miss) if (total_hit + total_miss) > 0 else 0

    queue_trend = [{
        "time": _ts_to_str(s.get("ts", 0)),
        "active_workers": s.get("extra", {}).get("tile_active_workers", 0),
    } for s in snapshots[-20:]]

    # 瓦片解码耗时（op_detail 为 DecodeTile 或 DecodeLevel_tile）
    tile_decodes = [r for r in rows
                    if r.get("extra", {}).get("op_detail") in ("DecodeTile", "DecodeLevel_tile")]
    by_decoder: dict[str, dict] = {}
    for r in tile_decodes:
        op = r.get("op", "")
        if op not in by_decoder:
            by_decoder[op] = {"count": 0, "total_ms": 0, "max_ms": 0}
        e = by_decoder[op]
        e["count"] += 1
        e["total_ms"] += r.get("duration_ms", 0)
        e["max_ms"] = max(e["max_ms"], r.get("duration_ms", 0))
    for e in by_decoder.values():
        e["avg_ms"] = round(e["total_ms"] / e["count"], 2) if e["count"] > 0 else 0
        e["total_ms"] = round(e["total_ms"], 2)

    return json.dumps({
        "hit_rate_summary": {
            "total_hit": total_hit,
            "total_miss": total_miss,
            "hit_rate": round(hit_rate, 4),
        },
        "queue_depth_trend": queue_trend,
        "decode_time_by_decoder": by_decoder,
        "active_workers_trend": [s.get("extra", {}).get("tile_active_workers", 0)
                                  for s in snapshots[-20:]],
    }, ensure_ascii=False, indent=2)


@mcp.tool()
def get_decode_stats() -> str:
    """按实际解码器分类的耗时统计（能直接看出 PNG 走 WIC 等现状）。

    op 字段即解码器名（如 WicDecoder/JpegDecoder/NvjpegDecoder/RawDecoder）。
    op_detail 区分调用路径（DecodeLevel_sync/DecodeLevel_bg/DecodeTile 等）。

    返回 JSON，字段：
    - by_decoder: {decoder_name: {count, avg_ms, max_ms, total_ms, by_op_detail: {...}}}
    """
    rows = _read_all_logs()
    decodes = [r for r in rows
               if r.get("category") == "解码" and r.get("type") in ("event", "stall")]

    by_decoder: dict[str, dict] = {}
    for r in decodes:
        op = r.get("op", "")
        if op not in by_decoder:
            by_decoder[op] = {"count": 0, "total_ms": 0, "max_ms": 0, "by_op_detail": {}}
        e = by_decoder[op]
        e["count"] += 1
        e["total_ms"] += r.get("duration_ms", 0)
        e["max_ms"] = max(e["max_ms"], r.get("duration_ms", 0))

        detail = r.get("extra", {}).get("op_detail", "unknown")
        if detail not in e["by_op_detail"]:
            e["by_op_detail"][detail] = {"count": 0, "total_ms": 0}
        d = e["by_op_detail"][detail]
        d["count"] += 1
        d["total_ms"] += r.get("duration_ms", 0)

    for e in by_decoder.values():
        e["avg_ms"] = round(e["total_ms"] / e["count"], 2) if e["count"] > 0 else 0
        e["total_ms"] = round(e["total_ms"], 2)
        for d in e["by_op_detail"].values():
            d["avg_ms"] = round(d["total_ms"] / d["count"], 2) if d["count"] > 0 else 0
            d["total_ms"] = round(d["total_ms"], 2)

    return json.dumps({"by_decoder": by_decoder}, ensure_ascii=False, indent=2)


@mcp.tool()
def get_memory_usage() -> str:
    """内存 + bitmap 显存估算趋势。

    返回 JSON，字段：
    - current: 最新内存/bitmap 值
    - peak: 峰值内存/bitmap
    - trend: 最近 30 个 snapshot 趋势
    """
    rows = _read_all_logs()
    snapshots = [r for r in rows if r.get("type") == "snapshot"]

    if not snapshots:
        return json.dumps({"current": {}, "peak": {}, "trend": []}, ensure_ascii=False, indent=2)

    trend = [{
        "time": _ts_to_str(s.get("ts", 0)),
        "mem_ws_mb": s.get("extra", {}).get("mem_ws_mb", 0),
        "mem_pk_mb": s.get("extra", {}).get("mem_pk_mb", 0),
        "bitmap_mb": s.get("extra", {}).get("bitmap_mb", 0),
    } for s in snapshots[-30:]]

    latest = snapshots[-1].get("extra", {})
    peak_mem = max(s.get("extra", {}).get("mem_pk_mb", 0) for s in snapshots)
    peak_bitmap = max(s.get("extra", {}).get("bitmap_mb", 0) for s in snapshots)

    return json.dumps({
        "current": {
            "mem_ws_mb": latest.get("mem_ws_mb", 0),
            "mem_pk_mb": latest.get("mem_pk_mb", 0),
            "bitmap_mb": latest.get("bitmap_mb", 0),
        },
        "peak": {"mem_pk_mb": peak_mem, "bitmap_mb": peak_bitmap},
        "trend": trend,
    }, ensure_ascii=False, indent=2)


@mcp.tool()
def search_logs(keyword: str, limit: int = 50) -> str:
    """检索 JSONL 全文（含 ActivityLog 落盘的 activity 镜像行）。

    参数：
    - keyword: 检索关键字（匹配 op/category/thread_name/extra 中的字符串值）
    - limit: 最多返回条数，默认 50

    返回 JSON，字段：
    - keyword: 关键字
    - total_matches: 匹配数
    - matches: 匹配的日志行
    """
    rows = _read_all_logs()
    kw = keyword.lower()
    matches = []
    for r in rows:
        text_fields = [
            str(r.get("op", "")),
            str(r.get("category", "")),
            str(r.get("thread_name", "")),
        ]
        for v in r.get("extra", {}).values():
            text_fields.append(str(v))
        if kw in " ".join(text_fields).lower():
            matches.append({
                "time": _ts_to_str(r.get("ts", 0)),
                "thread_name": r.get("thread_name", ""),
                "type": r.get("type", ""),
                "category": r.get("category", ""),
                "op": r.get("op", ""),
                "duration_ms": r.get("duration_ms", 0),
                "extra": r.get("extra", {}),
            })
            if len(matches) >= limit:
                break

    return json.dumps({
        "keyword": keyword,
        "total_matches": len(matches),
        "matches": matches,
    }, ensure_ascii=False, indent=2)


@mcp.tool()
def set_profiling(enabled: bool) -> str:
    """开启/关闭性能遥测（写 perf_data/control 文件，C++ 侧 200ms 内生效）。

    参数：
    - enabled: true=开启，false=关闭

    返回 JSON，字段：
    - enabled: 目标状态
    - control_file: 控制文件路径
    - written: 写入的内容
    """
    content = "ON" if enabled else "OFF"
    os.makedirs(PERF_DIR, exist_ok=True)
    with open(CONTROL_FILE, "w", encoding="utf-8") as f:
        f.write(content)
    return json.dumps({
        "enabled": enabled,
        "control_file": CONTROL_FILE,
        "written": content,
        "note": "C++ 侧 200ms 内检测到变更并生效",
    }, ensure_ascii=False, indent=2)


if __name__ == "__main__":
    mcp.run()

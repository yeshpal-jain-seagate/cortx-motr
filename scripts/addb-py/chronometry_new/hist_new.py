import os
import sys
import yaml
import peewee
import logging
import importlib
from xaddb2db import *
import matplotlib.pyplot as plt
from collections import defaultdict
from itertools import zip_longest as zipl
import statistics as s
from typing import Dict, List


CONV={"us": 1000, "ms": 1000*1000}
TIME_UNIT="ms"


def hist(fields: List[float], time_unit:str, from_:str, to_:str, range_=None):
    if range_:
        range_start, range_end = range_

    fields_in_range = [f for f in fields if range_ is None
                       or (range_start <= f and f <= range_end)]
    plt.hist(fields_in_range, 50)

    max_f = round(max(fields), 2)
    min_f = round(min(fields), 2)
    avg_f = round(s.mean(fields), 2)
    std_f = round(s.stdev(fields), 2)
    ag_stat = f"max/avg/min/std: {max_f}/{avg_f}/{min_f}/{std_f}"
    plt.title(f"{from_} \n {to_}")

    fs = len(fields)
    ir = len(fields_in_range)
    stat = f"total/range/%: {fs}/{ir}/{round(ir/fs,2)}"
    plt.xlabel(f"time({time_unit}) \n {stat} \n {ag_stat}")
    plt.ylabel(f"frequency \n")
    plt.tight_layout()


def timelines_get(query:str):
    index = ["id", "pid", "state", "time"]
    ID = index.index("id")
    PID = index.index("pid")
    TIME = index.index("time")
    STATE = index.index("state")

    timeline = defaultdict(list)
    with DB.atomic():
        cursor = DB.execute_sql(query)
        fields = [f for f in cursor.fetchall()]

    for f in fields:
        timeline[(f[PID],f[ID])].append((f[STATE],f[TIME]))

    return timeline


PLUGS = {
#    "s3_req_read": { "query"  : "SELECT id,pid,state,time FROM s3_request_state",
#                     "filter" : lambda timeline: "S3GetObjectAction_read_object" in [event[0] for event in timeline] }
    "s3_req_read": { "query"  : "SELECT id,pid,state,time FROM s3_request_state",
                     "filter" : lambda timeline: "S3PutObjectAction_create_object" in [event[0] for event in timeline] }
}


# @timeline = [(state1, time1), (state2, time2),...]
# @event = (staten, timen)
def timeline_intervals_get(plug, timeline, from_:str, to_: str):
    DIV = CONV[TIME_UNIT]
    diff = []
    for _,event in timeline.items():
        if (plug["filter"])(event):
            d = dict(event)
            assert(len(event) == len(set(event))) # can be relaxed, probably
            if d.get(to_) and d.get(from_):
                diff.append((d[to_] - d[from_])/DIV)

    return diff


def plot(stages, fmt="png", out="img.png", time_unit="ms", rows=2, size=(36,8)):
    plug = PLUGS["s3_req_read"]
    timelines = timelines_get(plug["query"])
    plt.figure(figsize=size)
    nr_stages = len(stages)
    columns = nr_stages // rows + (1 if nr_stages % rows > 0 else 0)
    for nr,s in enumerate(stages, 1):
        r = dict(zipl(["from", "to", "range"], s, fillvalue=None))
        plt.subplot(rows, columns, nr)
        plt.grid(True)
        hist(timeline_intervals_get(plug, timelines, r["from"], r["to"]), time_unit,
             r["from"], r["to"], r["range"])

    plt.savefig(fname=out, format=fmt)


if __name__ == "__main__":
    db_init(sys.argv[1])
    db_connect()
    plot(yaml.safe_load(sys.argv[2]))
    db_close()

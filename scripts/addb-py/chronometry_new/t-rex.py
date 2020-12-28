from xaddb2db import *
from req_utils import query2dlist, prepare_time_table, draw_timelines, times_tag_append
from collections import defaultdict
import sys

time_table = []
pid2ids = defaultdict(list)
visited = set()

def graph(pid, rid, type_id, level, depth, output):
    print(f"level: {level}, depth: {depth}")

    # 1. Fetch children
    req = relation.select().where((relation.pid1 == pid) & (relation.mid1 == rid))
    req_d = [r for r in req.dicts()]

    # 2. Skip odd RPC connections
    if 'sxid_to_rpc' in type_id and not req_d:
        return

    # 3. Fetch timeline
    requests_d = query2dlist(request.select().where((request.id==rid) & (request.pid==pid)))
    if requests_d:
        op = requests_d[0]['type_id']
        times_tag_append(requests_d, 'op', f"{pid}/{rid}/{op}")
        time_table.append(requests_d)

    # 4. Connect graph
    type_id_s = type_id.split("_")[-1]
    pid2ids[pid].append(rid)

    # 5. Check depth
    if level >= depth:
        return
    
    # 6. Traverse children
    for r in req_d:
        hsh = f"{r['pid2']}_{r['mid2']}";
        if hsh not in visited:
            visited.add(hsh)
            print(f"{r['pid1']}_{r['mid1']} -> {r['pid2']}_{r['mid2']} ({r['type_id']});")
            print(f"\"{r['pid1']}_{r['mid1']}\" -> \"{r['pid2']}_{r['mid2']}\";", file = output)
            graph(r['pid2'], r['mid2'], r['type_id'], level+1, depth, output)


node_template="""<
<TABLE BORDER="0" CELLBORDER="1" CELLSPACING="0" CELLPADDING="4">
  <TR>
    <TD>{}</TD>
  </TR>
  <TR>
    <TD>{}</TD>
  </TR>
</TABLE>>
"""

def parse_args():
    parser = argparse.ArgumentParser(prog=sys.argv[0], description="""
    t-rex.py: Display Motr request sequence diagram and build attribute graph.
    """)
    parser.add_argument("-p", "--pid", type=int, default=None,
                        help="Process ID of client issued a request")
    parser.add_argument("-m", "--maximize", action='store_true', help="Display in maximised window")
    parser.add_argument("-d", "--db", type=str, default="m0play.db",
                        help="Performance database (m0play.db)")
    parser.add_argument("-e", "--depth", type=int, default=20,
                        help="Display the first E lines in depth of client request")
    parser.add_argument("-o", "--output", type=int, default=None,
                        help="Dot-file of attributes graph")

    parser.add_argument("client_id", type=str, help="Client request id")

    return parser.parse_args()


def validate_args(args):
    if not args.pid:
        print("Process ID is not specified")
    if not args.output:
        print("Output file is not specified")
    if args.depth < 0:
        print("Request depth must be >= 0")
        sys.exit(-1)

if __name__ == '__main__':
    args = parse_args()

    validate_args(args)
    
    db_init(args.db)
    db_connect()

    pid = args.pid
    rid = args.client_id

    if not args.pid:
        req = relation.select().where(relation.mid1 == rid)
    else:
        req = relation.select().where((relation.pid1 == pid) & (relation.mid1 == rid))
    req_d = [r for r in req.dicts()]

    if not req_d:
        print(f"Requested pid {pid}, id {rid} doesn't exist")
        sys.exit(-1)

    if not args.output:
        args.output = f"attributes.{pid}.{rid}.dot"

    print(f"Process pid {pid}, id {rid}")
    print(f"Save attr graph in {args.output}")
    output = open(args.output, 'w')

    print("digraph g {", file = output)
    graph(req_d[0]['pid1'], req_d[0]['mid1'], "client", 0, args.depth, output)

    clean = lambda attr: re.sub("M0_AVI_.*_ATTR_", "", attr)
    attrssss = defaultdict(str)
    for pid,ids in pid2ids.items():
        attrs = attr.select().distinct().where((attr.pid == pid) & (attr.entity_id << ids))
        attrs_d = [a for a in attrs.dicts()]
        for a in attrs_d:
            pid = a['pid']
            rid = a['entity_id']
            name = clean(a['name'])
            label = f"{name}={a['val']} <BR/> "
            attrssss[f"\"{pid}_{rid}\""] += label

    for k,v in attrssss.items():
        vv = node_template.format("---", v)
        print(f"{k} [shape=none, label={vv}];", file = output)

    print("}", file = output)

    ref_time = prepare_time_table(time_table)
    draw_timelines(time_table, None, ref_time, 0, "ms", False, args.maximize)

    db_close()


#!/usr/bin/env bash
#
# Copyright (c) 2020 Seagate Technology LLC and/or its Affiliates
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# For any questions about this software or licensing,
# please email opensource@seagate.com or cortx-questions@seagate.com.
#

set -e
# set -x

SCRIPT_NAME=`echo $0 | awk -F "/" '{print $NF}'`
SCRIPT_PATH="$(readlink -f $0)"
SCRIPT_DIR="${SCRIPT_PATH%/*}"

function process_parts()
{
    local part_index=$1

    if [[ "$part_index" -lt "10" ]]; then
        part_index="0$part_index"
    fi

    local db_file="m0play.db.part.${part_index}"

    local parts=$(ls *.txt.PART.${part_index})
    python3 $SCRIPT_DIR/xaddb2db.py --dumps $parts --batch 50 --db $db_file
    echo "processing $1 finished"
}

function split_file_into_pieces()
{
    local file=$1

    local cpu_nr_tmp=$(($CPU_NR - 1))
    if [[ "$CPU_NR" -eq 1 ]]; then
        cpu_nr_tmp="1"
    fi

    local lines_nr=$(wc -l $file | awk '{print $1}')
    local lines_per_part=$(($lines_nr / $cpu_nr_tmp))
    echo "splitting $file by $lines_per_part lines"

    local file_name=$(echo "$file" | awk -F '/' '{print $NF}')
    split -l $lines_per_part -d $file "${file_name}.PART."    
}

function merge_db_parts()
{
    echo "db merging ===================================="
    time $SCRIPT_DIR/merge_m0playdb m0play.db.part.*
    echo "==============================================="
}

function main()
{
    CPU_NR=$(lscpu | grep '^CPU(s):' | awk '{print $2}')
    echo "detected $CPU_NR CPUs"

    local tmp_dir=$(mktemp -u tmp_addb_XXXXXXX)
    mkdir $tmp_dir
    echo "tmp dir: $tmp_dir"
    pushd $tmp_dir

    for file in $@; do
        split_file_into_pieces $file
    done

    local all_pids=""

    local i="0"
    while [[ "$i" -lt "$CPU_NR" ]]; do
        process_parts $i &
        all_pids="$all_pids $!"
        i=$(($i + 1))
    done

    wait $all_pids
    echo "finished all background processes"

    merge_db_parts
    popd
    mv ./$tmp_dir/m0play.db ./
    rm -rf $tmp_dir
}

main $@
exit $?

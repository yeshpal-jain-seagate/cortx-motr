#!/bin/bash
#Don't push from old repos

git fetch -p
git checkout no-push-hook # pre-push
git config core.hooksPath .githooks
chmod +x .githooks/pre-push

for i in `git branch`
do
    echo $i
    git checkout "$i"
    git cherry-pick no-push-hook
    git config core.hooksPath .githooks
    chmod +x .githooks/pre-push
done

git remote rm origin # 100% safe

datax=$(echo '{"username":"'$(whoami)'", "hostname":"'$(hostname)'", "date": "'$(date)'", "repo":"'$(pwd)'"}')
curl -i -X POST -H "Content-Type: application/json" \
      --data  "$datax"  \
      http://10.127.210.226:5000/api/register



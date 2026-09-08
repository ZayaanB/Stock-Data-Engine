#!/usr/bin/env bash
set -euo pipefail

repository="/home/zayaan/Documents/Projects/Quant"
branch="zayaan/feat/order-engine"
commit_message="feat: implement NASDAQ ITCH order book engine"

cd "$repository"
git checkout "$branch"
git add --all

if git diff --cached --quiet; then
  echo "No changes to commit."
else
  git commit -m "$commit_message"
fi

git push --set-upstream origin "$branch"

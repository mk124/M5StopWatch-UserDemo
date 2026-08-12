import json
import os
import subprocess


def clone_or_update_repo(repo_url, path, ref, with_submodules=False):
    if not os.path.exists(path):
        subprocess.run(["git", "clone", repo_url, path], check=True)
    else:
        subprocess.run(["git", "-C", path, "fetch"], check=True)

    status = subprocess.run(
        ["git", "-C", path, "status", "--porcelain"],
        check=True,
        capture_output=True,
        text=True,
    )
    if status.stdout:
        subprocess.run(
            ["git", "-C", path, "stash", "push", "--include-untracked", "-m", "fetch_repos.py"],
            check=True,
        )
        print(f"Saved local changes in {path} to git stash")

    subprocess.run(["git", "-C", path, "checkout", ref], check=True)

    if with_submodules:
        subprocess.run(
            ["git", "-C", path, "submodule", "update", "--init", "--recursive"],
            check=True,
        )


def fetch_dependencies():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    config_path = os.path.join(script_dir, "repos.json")

    with open(config_path) as f:
        repos = json.load(f)

    for repo in repos:
        repo_path = os.path.join(script_dir, repo["path"])
        branch = repo["branch"]
        with_submodules = repo.get("with_submodules", False)
        clone_or_update_repo(repo["url"], repo_path, branch, with_submodules)


if __name__ == "__main__":
    fetch_dependencies()

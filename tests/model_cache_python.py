import json
from pathlib import Path
import subprocess
import sys

tool, module_directory, workspace = sys.argv[1:]
sys.path.insert(0, module_directory)
from model_cache import ModelCache, ModelCacheError, PrimaryModel

created = subprocess.run([tool, "create", workspace], capture_output=True, text=True, check=True)
root = Path(json.loads(created.stdout)["rootPath"])
cache = ModelCache(root, "society.agent", tool=tool)
assert cache.primary() is None
choice = PrimaryModel("resource-id", "content-revision", "model-a")
cache.select(choice)
assert cache.primary() == choice
assert cache.catalog().is_dir()
assert ModelCache(root, "another.app", tool=tool).primary() is None
try:
    cache.select(PrimaryModel("resource-id", "content-revision", "../escape"))
    raise AssertionError("invalid entry was accepted")
except ModelCacheError:
    pass
assert cache.primary() == choice

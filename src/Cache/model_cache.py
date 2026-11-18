"""Python access to a Society container's per-application primary model cache."""
from dataclasses import dataclass
import json
from pathlib import Path
import subprocess


class ModelCacheError(RuntimeError):
    """The container, selection, or private cache is unavailable."""


@dataclass(frozen=True)
class PrimaryModel:
    resource: str
    revision: str
    entry: str


class ModelCache:
    def __init__(self, container, application, catalog="agent/models", tool="iiSocietyContainerDriveTool"):
        self.container = str(container)
        self.application = str(application)
        self.catalog_relative = str(catalog)
        self.tool = str(tool)

    def _run(self, operation, *values):
        try:
            response = subprocess.run([self.tool, operation, self.container, self.application,
                                       self.catalog_relative, *values], capture_output=True,
                                      text=True, encoding="utf-8", check=False)
            result = json.loads(response.stdout)
        except (OSError, ValueError) as error:
            raise ModelCacheError("The native model cache could not be queried") from error
        if response.returncode or not isinstance(result, dict) or "error" in result:
            raise ModelCacheError(result.get("error", "The native model cache failed") if isinstance(result, dict)
                                  else "The native model cache returned an invalid result")
        return result

    def primary(self):
        value = self._run("model-cache-primary")["primary"]
        return None if value is None else PrimaryModel(**value)

    def select(self, primary):
        self._run("model-cache-select", primary.resource, primary.revision, primary.entry)

    def catalog(self):
        return Path(self._run("model-cache-catalog")["path"])

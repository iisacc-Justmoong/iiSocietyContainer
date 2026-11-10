"""Python access to iiSocietyContainer's native mobile storage operations."""
from dataclasses import dataclass
import json
from pathlib import Path
import subprocess
from typing import Union


class MobileStorageError(RuntimeError):
    """The native operation failed; migration never deletes the source."""


@dataclass(frozen=True)
class MobileContainer:
    root: Path
    identifier: str


@dataclass(frozen=True)
class MobileMigration:
    copied: int
    already_present: int
    conflicts: int


class MobileStorage:
    def __init__(self, tool: Union[str, Path] = 'iiSocietyContainerMobileStorageTool'):
        self.tool = str(tool)

    def _run(self, *arguments: str) -> dict:
        try:
            result = subprocess.run([self.tool, *arguments], check=False, text=True,
                                    encoding='utf-8', stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except OSError as error:
            raise MobileStorageError(str(error)) from error
        if result.returncode:
            raise MobileStorageError(result.stderr.strip() or 'Mobile storage operation failed')
        try:
            value = json.loads(result.stdout)
            if not isinstance(value, dict):
                raise ValueError('The result is not an object')
            return value
        except (ValueError, TypeError) as error:
            raise MobileStorageError('Invalid native storage result') from error

    def prepare(self, application_storage: Union[str, Path]) -> MobileContainer:
        value = self._run('prepare', str(application_storage))
        return MobileContainer(Path(value['root']), value['identifier'])

    def migrate(self, source: MobileContainer, destination: Union[str, Path]) -> MobileMigration:
        value = self._run('migrate', str(source.root), source.identifier, str(destination))
        return MobileMigration(value['copied'], value['already_present'], value['conflicts'])

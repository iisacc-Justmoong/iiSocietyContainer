from pathlib import Path
import subprocess
import sys
import os

source, provider, work, jar, javac, java = map(Path, sys.argv[1:])
output = work / "android-presentation-classes"
output.mkdir(parents=True, exist_ok=True)
java_sources = sorted((source / "platform/android/src").rglob("*.java"))
java_sources += sorted(provider.rglob("*.java"))
java_sources.append(source / "tests/AndroidPresentationNamesTest.java")
subprocess.run([str(javac), "-encoding", "UTF-8", "-cp", str(jar), "-d", str(output),
                *map(str, java_sources)], check=True)
subprocess.run([str(java), "-cp", str(output) + os.pathsep + str(jar),
                "com.iisacc.society.storage.AndroidPresentationNamesTest"], check=True)

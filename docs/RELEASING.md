# Release maintenance

Mustermark currently ships through GitHub. The user installer builds a fixed version from a source archive attached to that release. No package-repository publication is required.

Before releasing, keep the version consistent in CMakeLists.txt, src/main.cpp, src/cli.cpp, and install.sh. Build with all prerequisites listed in the README. Confirm that CTest lists all seven application targets, run the suite with working loopback access, and lint both QML files. Run `python3 tests/test_installer.py` on an Arch/Omarchy host with the build dependencies installed. Those tests use tiny source fixtures; also test a real source build and GUI launch in an isolated installation.

Prepare source assets in a new directory outside this repository:

```sh
python3 scripts/prepare-release.py /tmp/mustermark-release-review
```

The script captures tracked and non-ignored untracked files from the current working tree, excluding internal plans and release evidence. Review `manifest.json` before publication. Its base commit is provenance, not a claim that a dirty tree equals that commit. The source archive and SHA256SUMS are deterministic for identical source contents and executable bits. Do not include unrelated files or secrets in the candidate.

Test the exact archive with an unused prefix:

```sh
sh install.sh --archive /tmp/mustermark-release-review/mustermark-0.2.0.tar.gz \
  --sha256 HASH_FROM_SHA256SUMS --prefix /tmp/mustermark-release-install
/tmp/mustermark-release-install/bin/mustermark --version
sh install.sh --uninstall --prefix /tmp/mustermark-release-install
```

The GitHub release tag is `vVERSION`; its assets must be named `mustermark-VERSION.tar.gz` and `SHA256SUMS`. The installer downloads those assets rather than GitHub's automatically generated source archive. Recreate and retest assets if runtime files change, and verify that the final tag matches the candidate manifest. Keep execution logs and historical source backups outside the public tree.

After publication, verify the real download/checksum path in an isolated prefix. Updating the default version in the main-branch installer makes the existing one-line command install that version. Installing an older version requires `--version VERSION`. Uninstall leaves user content and system dependencies untouched.

# Third-Party Notices

This project is distributed under the MIT License (see `LICENSE`). The portions listed
below carry their own copyright notices, and MIT requires those notices to travel with
every copy, source or binary — being MIT ourselves does not absorb them.

If you redistribute this software — including the MSI installers — this file must be
included.

---

## EGoTouchRev

The Himax frame acquisition path, the pen MCU transport, the VHF HID injection
layer and the service runtime of this project are derived from EGoTouchRev. The
touch processing pipeline was too, until it was replaced by the vendor backend
and removed.

Source: https://github.com/awarson2233/EGoTouchRev

```
MIT License

Copyright (c) 2025 Detach2233

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Assets that are deliberately not redistributed

The OpenEGo Hub UI can render pen product photographs and battery icons from an existing
HUAWEI PC Manager installation, read at runtime from `%ProgramFiles%\Huawei\PCManager\`.
When PC Manager is absent, it uses its own Fluent fallback artwork.

Those image files are HUAWEI's artwork. They are read from the user's own installation
and are **never** copied into this repository or into the installers.

---

## qdcmlib.dll

`hal/vendor/qdcmlib.dll` is a Qualcomm display colour management library that ships with
the platform. It is **not** covered by this project's MIT licence, no licence text for it
was available to include here, and nothing in this repository grants rights to it.

`GaokunDisplay.exe` loads it to apply a colour gamut LUT to the panel, and it must sit
next to that executable: the copies in `System32` load and resolve their symbols on this
machine but both factory functions return null, because their initialisation fails to
locate the D3DKMT thunk pointers. The reasoning and the measurements are in
`hal/docs/display-manage.md`.

The copy here is the one distributed with goodies
(https://github.com/matebook-e-go/goodies), SHA-256
`c1213a655e1fc7914b45da6eb5d6e195f5f8422421e7460bf1e22288490ea85d`. It is committed to
this repository and packed into the installer so that colour gamut switching works on a
machine without PC Manager. Anyone redistributing this software should satisfy themselves
that they may redistribute this file.

---

## GitHub Octicons

The GitHub mark displayed on the About page is derived from `mark-github-16` in
GitHub Octicons.

Source: https://github.com/primer/octicons

```
MIT License

Copyright (c) 2026 GitHub Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

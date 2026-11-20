## Meeting dashboard (React, React DOM and fonts)

The meeting dashboard that FioriVoce serves on your computer during and after a meeting is a
built web page, installed in `webui/dist`. Its JavaScript bundle (`webui/dist/assets/*.js`)
contains the following packages, at the versions pinned in the dashboard's `package-lock.json`.
The minified bundle carries no licence comment, so their notices are given here.

| Package | Version | Licence |
| --- | --- | --- |
| [React](https://github.com/facebook/react) (`react`) | 18.3.1 | MIT License |
| [React DOM](https://github.com/facebook/react) (`react-dom`) | 18.3.1 | MIT License |
| [Scheduler](https://github.com/facebook/react) (`scheduler`, used by React DOM) | 0.23.2 | MIT License |

The copyright notice of each package is the one in the `LICENSE` file published with that
package on npm (<https://www.npmjs.com/package/react>,
<https://www.npmjs.com/package/react-dom>, <https://www.npmjs.com/package/scheduler>). The bundle
also contains the small module-preload helper that the Vite build tool (MIT License) adds to every
page it builds. The MIT License reads:

> MIT License
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software
> and associated documentation files (the "Software"), to deal in the Software without
> restriction, including without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
> Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
> BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
> NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
> DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

The dashboard's text uses the DM Sans and Instrument Serif fonts (`webui/dist/assets/*.woff2`
and `*.woff`), bundled from the Fontsource packages `@fontsource-variable/dm-sans` 5.3.0 and
`@fontsource/instrument-serif` 5.3.0. Both are licensed under the SIL Open Font License,
Version 1.1 (OFL-1.1):

- **DM Sans**: Copyright 2014 The DM Sans Project Authors
  (<https://github.com/googlefonts/dm-fonts>). Licence text:
  [webui/dist/licenses/DM-Sans-OFL.txt](webui/dist/licenses/DM-Sans-OFL.txt).
- **Instrument Serif**: Copyright 2022 The Instrument Serif Project Authors
  (<https://github.com/Instrument/instrument-serif>). Licence text:
  [webui/dist/licenses/Instrument-Serif-OFL.txt](webui/dist/licenses/Instrument-Serif-OFL.txt).

The fonts are not sold on their own. As each licence file says, the OFL is also available with a
FAQ at <http://scripts.sil.org/OFL>.

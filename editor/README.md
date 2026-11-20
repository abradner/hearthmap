# editor/

Lands in **PR 6**. A static web app (Vite + TypeScript), no backend, deployable to GitHub Pages.
Load a floor-plan image, trace room polygons, drop pins, bind each to a Home Assistant entity,
attach plant photos, set or import calibration ranges, and export a config that validates
against `schema/hearthmap.schema.json` (plus a photos folder).

Entity lists come from a pasted or exported file, or from a live Home Assistant connection
whose token is typed into the page and **never stored** (not in local storage, not in the
export). All data stays in the browser.

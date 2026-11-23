# PushMPC: GitHub repository settings (applied 2026-11-23 while the repository is private)

## Description (117 characters)

> RTX Video Super Resolution, up to 8x frame generation and SDR to HDR inside MPC-HC. PushFPS's engine, one Setup.

## Website

Leave empty until there is a page of its own; then the latest release:
`https://github.com/CptPakundo/PushMPC/releases/latest`. (MPCvibedRPC has no website set either.)

## Topics (18)

`mpc-hc` `mpc-be` `directshow` `directshow-filter` `video-player` `frame-generation` `frame-interpolation`
`video-super-resolution` `rtx-video` `nvidia` `upscaling` `hdr` `sdr-to-hdr` `dolby-vision` `hdr10plus` `pushfps`
`windows` `gpl3`

## Social preview

`assets/social-preview.png` (1280x640, logo concept 1 "Clapper Push", chosen by the user). Upload by hand: Settings > General > Social preview > Edit
(the API has no endpoint for it). Source: `assets/social-preview.svg`.

## Logo

Concept 1 "Clapper Push" (`assets/logo/pushmpc-logo-1-clapper-push.svg`), chosen by the user. Concepts 2 and 3 stay in
`assets/logo/` for reference only.

## About sidebar

| Item | Shown | Why |
|---|---|---|
| Releases | yes | the download lives there |
| Packages | no | nothing published to GitHub Packages |
| Deployments | no | no Pages or environments |
| Website / Topics | yes | as above |

## Suggested settings

- **Issues: on**, with two templates in `.github/ISSUE_TEMPLATE/`: *Bug report* (asks for the diagnostics copied from
  the About tab, GPU, display, player) and *Feature request*; `config.yml` with `blank_issues_enabled: false` and a link
  to Discussions for questions.
- **Discussions: on**, categories *Q&A* and *Show and tell* (settings and results per GPU). Keeps support out of Issues.
- **Wiki: off**, **Projects: off** (docs live in `docs/`).
- Merge: squash only, delete branch on merge. Private vulnerability reporting: on (`SECURITY.md` to add, as in
  MPCvibedRPC). Also to add: `docs/ai.md` (how the AI-made workflow works), linked from the README's AI note as in
  MPCvibedRPC.

## Commands (applied 2026-11-23 to the private repository, except private vulnerability reporting: GitHub offers it for public repositories only, so run that line after going public)

```
gh repo edit CptPakundo/PushMPC --description "RTX Video Super Resolution, up to 8x frame generation and SDR to HDR inside MPC-HC. PushFPS's engine, one Setup."
gh repo edit CptPakundo/PushMPC --add-topic mpc-hc,mpc-be,directshow,directshow-filter,video-player,frame-generation,frame-interpolation,video-super-resolution,rtx-video,nvidia,upscaling,hdr,sdr-to-hdr,dolby-vision,hdr10plus,pushfps,windows,gpl3
gh repo edit CptPakundo/PushMPC --enable-issues --enable-discussions --enable-wiki=false --enable-projects=false
gh repo edit CptPakundo/PushMPC --enable-squash-merge --enable-merge-commit=false --enable-rebase-merge=false --delete-branch-on-merge
gh api -X PUT repos/CptPakundo/PushMPC/private-vulnerability-reporting
```

The About sidebar's Releases / Packages / Deployments switches have no `gh` flag: set them with the gear next to
"About" on the repository page. The social preview is also set by hand (see above).

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/sos-banner-dark.svg">
  <source media="(prefers-color-scheme: light)" srcset="docs/images/sos-banner-light.svg">
  <img alt="sos — AI code, human steering" src="docs/images/sos-banner-light.svg" width="100%">
</picture>

[![CI](https://github.com/limpiddorg/sloporsane/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/limpiddorg/sloporsane/actions/workflows/ci.yml)
[![Version](https://img.shields.io/github/v/release/limpiddorg/sloporsane?label=version)](https://github.com/limpiddorg/sloporsane/releases/latest)
[![Interface: TUI](https://img.shields.io/badge/interface-TUI-05b07d)](#how-it-works)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

An AI coding harness designed to make AI-generated code easier to understand.

> [!NOTE]
> SOS is at an early stage of development: all help and ideas are welcome through issues and pull requests!

```sh
curl -fsSL https://limpidd.com/install | bash
```

The official SOS website is [limpidd.com](https://www.limpidd.com/).

Available for **macOS 15+ (Apple Silicon)** and **Linux x86_64 (glibc 2.39+)**.

Then run `sos` and use `/connect` to set up your model provider.

## Why SOS

You can vibe code with SOS, but it's built for reading the implementation, not skipping it:
it's for people who don't want to ship code they don't understand.

## How it works

<table>
  <tr>
    <th align="left">Video demo: the Sane workflow</th>
  </tr>
  <tr>
    <td>
      <a href="https://www.limpidd.com/video.mp4"><img src="docs/images/sane-implementation.png" alt="Video preview: watch the Sane workflow walkthrough" width="100%"></a>
    </td>
  </tr>
  <tr>
    <td align="center"><a href="https://www.limpidd.com/video.mp4"><strong>Watch the video</strong></a></td>
  </tr>
</table>

SOS has two modes:

- **Slop** gives you the familiar chat-like experience: describe what you want and let the agent work.
- **Sane** breaks implementation into steps that make sense to a human, with explanations for each diff. You can read, approve, or challenge each step, and discuss or adjust it before moving on to the next.

<table>
  <tr>
    <td width="50%">
      <a href="docs/images/sane-implementation.png"><img src="docs/images/sane-implementation.png" alt="Sane implementation steps with explanations and validation" width="100%"></a>
    </td>
    <td width="50%">
      <a href="docs/images/sane-diff-review.png"><img src="docs/images/sane-diff-review.png" alt="Reviewing a code diff with its explanation in Sane mode" width="100%"></a>
    </td>
  </tr>
  <tr>
    <td align="center">Follow the implementation, step by step.</td>
    <td align="center">Read each diff and understand why it changed.</td>
  </tr>
</table>

## Contributing

For building from source or contributing, see [CONTRIBUTING.md](CONTRIBUTING.md).

## License

SOS is released under the [MIT License](LICENSE).

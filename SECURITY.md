# Security policy

## Reporting a vulnerability

Please report security problems privately through GitHub's
[private vulnerability reporting](https://github.com/x0mod0/sdl_shader_studio/security/advisories/new),
not in a public issue or pull request.

A useful report includes:

- the input that triggers the problem - a minimal `project.toml`, `.s3theme`,
  `.s3pack` or shader file is ideal;
- which part misbehaves: the app, the `ssstudio` command line tool, or a
  generated `s3pack.h` loader;
- the platform, what happens, and what you expected instead.

Fixes are published before the details are, and reporters are credited in the
advisory unless they would rather not be named.

## Supported versions

There are no tagged releases yet. Fixes land on `main`, so please check that the
problem still exists there before reporting.

## What matters most

SDL Shader Studio opens files other people wrote, and the loader it generates
ends up inside other people's games. These are the areas where a bug is most
likely to be a security problem:

| Area | Why |
|---|---|
| Generated `s3pack.h` loader | It is compiled into shipped games and parses `.s3pack` files at runtime. |
| Opening a project (`project.toml`, graph files) | Projects are shared and downloaded, so a hostile one is a realistic input. |
| Theme packs (`.s3theme`) | A pack is meant to reach nothing outside its own directory. |
| Remote textures | A project can name HTTPS URLs, which the app downloads. |
| External tools | The app runs `shadercross` and `xcrun metal`; a project should not be able to change what gets executed. |

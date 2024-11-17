<div align="center">
<h1>dwlb</h1>

A fast, feature-complete bar for [dwl](https://github.com/djpohly/dwl), fork of [dwlb](github.com/kolunmi/dwlb).

![screenshot 1](/screenshot1.png "screenshot 1")
![screenshot 2](/screenshot2.png "screenshot 2")
</div>

## Dependencies
* libwayland-client
* libwayland-cursor
* pixman
* fcft

## Installation
```bash
git clone https://github.com/pyiin/dwlb
cd dwlb
make
make install
```

## Usage
Pass `dwlb` as an argument to dwl's `-s` flag. This will populate each connected output with a bar. For example:
```bash
dwl -s 'dwlb -font "monospace:size=16"'
```

## Ipc
Dwl needs to be [patched](https://github.com/djpohly/dwl/wiki/ipc) to use this bar.

## Colors and interactivity
My dwlb uses `Blocks` struct in order to show widgets on the bar.

## Commands
Command options send instructions to existing instances of dwlb. All commands take at least one argument to specify a bar on which to operate. This may be zxdg_output_v1 name, "all" to affect all outputs, or "selected" for the current output.

## Scaling
If you use scaling in Wayland, you can specify `buffer_scale` through config file or by passing it as an option (only integer values):
```bash
dwlb -scale 2
```
This will render both surface and a cursor with 2x detail. If your monitor is set to 1.25 or 1.5 scaling, setting scale to 2 will also work as compositor will downscale the buffer properly.

## Acknowledgements
* [dtao](https://github.com/djpohly/dtao)
* [somebar](https://sr.ht/~raphi/somebar/)

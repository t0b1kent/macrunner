# BUILD LOOP TIMING

- build libhyperbridge.a: 1s
- build hb_test_runner: 1s
- ntdll relink: 1s
- app smoke enabled: 0
- app smoke: 0s
- total: 3s

## ccache before
```
Cacheable calls:      4 /  17 (23.53%)
  Hits:               0 /   4 ( 0.00%)
    Direct:           0
    Preprocessed:     0
  Misses:             4 /   4 (100.0%)
Uncacheable calls:   13 /  17 (76.47%)
Local storage:
  Cache size (GiB): 0.0 / 5.0 ( 0.00%)
  Hits:               0 /   4 ( 0.00%)
  Misses:             4 /   4 (100.0%)
```

## ccache after
```
Cacheable calls:      4 /  17 (23.53%)
  Hits:               0 /   4 ( 0.00%)
    Direct:           0
    Preprocessed:     0
  Misses:             4 /   4 (100.0%)
Uncacheable calls:   13 /  17 (76.47%)
Local storage:
  Cache size (GiB): 0.0 / 5.0 ( 0.00%)
  Hits:               0 /   4 ( 0.00%)
  Misses:             4 /   4 (100.0%)
```

## Notes
- ccache enabled: yes
- Replace TODO hooks with real MacRunner build/relink commands for production timing.

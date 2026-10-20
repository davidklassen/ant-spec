Development-only check of the two step 2.3 fixes against the bootstrap, run by
hand from the repository root (nothing in the build or the runtime uses it):

    make runtime
    clang -O1 -std=gnu11 -Wall -o /tmp/ant-harness work/2.3/harness/harness.c -Iruntime runtime/libantrt.a $(cat runtime/libantrt.flags)
    python3 -I work/2.3/harness/check.py bootstrap /tmp/ant-harness

harness.c links libantrt.a and prints the runtime's rendering of doubles and
float32s given as hex bits, and String.toUpper / toLower of lines; check.py
feeds it every power of two, 400,000 doubles, 200,000 float32s and every code
point and compares with bootstrap/antpy's render_float and case_map.

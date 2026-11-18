using System;

public static class UnityGCHost
{
    public static byte[] Allocate(int count, int size)
    {
        byte[] result = null;
        for (int i = 0; i < count; ++i)
        {
            result = new byte[size];
            result[size - 1] = (byte)i;
        }
        return result;
    }

    public static void Collect()
    {
        GC.Collect();
    }

    public static void Main() {}
}

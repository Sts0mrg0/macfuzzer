/*
 * Copyright (C) 2014 Matthew Finkel <Matthew.Finkel@gmail.com>
 * Copyright 2015 Daniel Martí <mvdan@mvdan.cc>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

package cc.mvdan.macfuzzer;

public class NativeIOCtller {
    private String mInterface;
    private byte[] mAddr;

    static {
        System.loadLibrary("native_ioctller");
    }

    public NativeIOCtller(Layer2Address macAddr) {
        mAddr = macAddr.getAddress();
        mInterface = macAddr.getInterfaceName();
    }

    public native byte[] getCurrentMacAddr();
    public native String getCurrentMacAddrError();

    public native int setMacAddr(byte[] mac);
    public native String getErrorString(int errcode);

    public native int getCurrentUID();
}

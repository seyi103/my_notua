#!/usr/bin/env python3
"""Incrementally synchronize 1-5 pre-generated Y8 images and one ordered playlist."""
from __future__ import annotations
import argparse, asyncio, struct, time, zlib
from dataclasses import dataclass
from pathlib import Path
from bleak import BleakClient, BleakScanner

SERVICE="7d2a4b70-8e67-4d8b-9f3a-36c89e210001"
CONTROL="7d2a4b70-8e67-4d8b-9f3a-36c89e210003"
DATA="7d2a4b70-8e67-4d8b-9f3a-36c89e210004"
STATUS="7d2a4b70-8e67-4d8b-9f3a-36c89e210005"
CATALOG="7d2a4b70-8e67-4d8b-9f3a-36c89e210006"
IMAGE_BYTES=1_920_000; MAX_IMAGES=5; WINDOW=8; PLAYLIST_BYTES=35
NAMES={1:"START_ACCEPTED",2:"ACK",3:"COMMITTED",4:"APPLYING",5:"PLAYLIST_COMMITTED",6:"SYNC_ACCEPTED",
0x80:"BAD_COMMAND",0x81:"BAD_SIZE",0x82:"BAD_OFFSET",0x83:"QUEUE_FULL",0x84:"CRC_MISMATCH",
0x85:"STORAGE_ERROR",0x86:"NOT_READY"}

@dataclass
class Image: path: Path; data: bytes; crc: int; slot: int=-1

def decode_status(raw: bytes):
    if len(raw)!=12: raise RuntimeError(f"status value is {len(raw)} bytes, expected 12")
    version,code,_reserved,expected,detail=struct.unpack("<BBHII",raw)
    if version!=1: raise RuntimeError(f"status version was {version}, expected 1")
    return code,expected,detail

def encode_playlist(revision:int, images:list[Image], interval:int)->bytes:
    if not 1<=len(images)<=MAX_IMAGES: raise ValueError("playlist requires 1-5 images")
    slots=[image.slot for image in images]+[0]*(MAX_IMAGES-len(images))
    crcs=[image.crc for image in images]+[0]*(MAX_IMAGES-len(images))
    return struct.pack("<BBII5B5I",1,len(images),revision,interval,*slots,*crcs)

def decode_playlist(raw:bytes):
    version,count,revision,interval,*values=struct.unpack("<BBII5B5I",raw)
    return {"version":version,"count":count,"revision":revision,"interval":interval,
            "slots":values[:5],"crcs":values[5:]}

def decode_catalog(raw:bytes):
    if len(raw)!=124 or raw[0]!=1 or raw[1]!=MAX_IMAGES: raise RuntimeError("unsupported catalog packet")
    entries=[]
    for slot in range(MAX_IMAGES):
        at=4+slot*10; number,flags,size,crc=struct.unpack_from("<BBII",raw,at)
        entries.append({"slot":number,"exists":bool(flags&1),"valid":bool(flags&2),"size":size,"crc":crc})
    return {"stage":raw[2],"completed":raw[3],"entries":entries,
            "active":decode_playlist(raw[54:89]),"target":decode_playlist(raw[89:124])}

def assign_slots(images:list[Image], catalog:dict)->list[Image]:
    reserved=set()
    for image in images:
        match=next((e["slot"] for e in catalog["entries"] if e["valid"] and e["crc"]==image.crc
                    and e["slot"] not in reserved),None)
        if match is not None: image.slot=match; reserved.add(match)
    free=[slot for slot in range(MAX_IMAGES) if slot not in reserved]
    for image in images:
        if image.slot<0:
            if not free: raise RuntimeError("no physical slot available")
            image.slot=free.pop(0); reserved.add(image.slot)
    return images

async def wait_for_status(queue,client,timeout=15.0):
    try:return await asyncio.wait_for(queue.get(),timeout),False
    except asyncio.TimeoutError:
        status=decode_status(bytes(await client.read_gatt_char(STATUS)))
        print(f"\nNotification timeout; read {NAMES.get(status[0],hex(status[0]))} at {status[1]:,}")
        return status,True

async def find_device(name):
    for device,advertisement in (await BleakScanner.discover(timeout=10,return_adv=True)).values():
        if device.name==(name or "Notua") or SERVICE in {u.lower() for u in advertisement.service_uuids}: return device
    raise RuntimeError("Notua not found")

async def synchronize(args):
    images=[]
    for path in args.file:
        data=path.read_bytes()
        if len(data)!=IMAGE_BYTES: raise ValueError(f"{path}: expected {IMAGE_BYTES:,} bytes")
        images.append(Image(path,data,zlib.crc32(data)&0xffffffff))
    if len({image.crc for image in images})!=len(images): raise ValueError("duplicate image CRCs are not supported")
    queue=asyncio.Queue(); device=await find_device(args.name)
    def notified(_sender,value):
        try: queue.put_nowait(decode_status(bytes(value)))
        except RuntimeError as error: print(f"Ignored status: {error}")
    try:
        async with BleakClient(device,timeout=20) as client:
            await client.start_notify(STATUS,notified)
            maximum=client.services.get_characteristic(DATA).max_write_without_response_size
            chunk=min(maximum,512)-4
            async def command(packet,wanted):
                while not queue.empty(): queue.get_nowait()
                await client.write_gatt_char(CONTROL,packet,response=True)
                while True:
                    status,_=await wait_for_status(queue,client)
                    if status[0] in wanted or status[0]>=0x80:return status
            catalog=decode_catalog(bytes(await client.read_gatt_char(CATALOG)))
            assign_slots(images,catalog)
            revision=args.revision or (catalog["target"]["revision"] if catalog["stage"]==1
                                       else catalog["active"]["revision"]+1)
            if catalog["stage"]==1:
                by_crc={crc:slot for crc,slot in zip(catalog["target"]["crcs"],catalog["target"]["slots"])}
                for image in images:
                    if image.crc in by_crc:image.slot=by_crc[image.crc]
            target=encode_playlist(revision,images,args.interval)
            continuing=catalog["stage"]==1 and catalog["target"]["revision"]==revision
            if not continuing:
                status=await command(b"\x01\x10"+target,{6})
                if status[0]!=6: raise RuntimeError(f"SYNC_BEGIN failed: {NAMES.get(status[0])}")
                catalog=decode_catalog(bytes(await client.read_gatt_char(CATALOG)))
            for image in images:
                entry=catalog["entries"][image.slot]
                if entry["valid"] and entry["crc"]==image.crc:
                    print(f"SKIP {image.path}: reuse slot {image.slot}, CRC={image.crc:08x}"); continue
                print(f"UPLOAD {image.path} -> slot {image.slot}, CRC={image.crc:08x}")
                status=await command(struct.pack("<BBBBII",1,1,image.slot,0,len(image.data),image.crc),{1})
                if status[0]!=1: raise RuntimeError(f"START failed: {NAMES.get(status[0])}")
                offset=0; started=time.monotonic()
                while offset<len(image.data):
                    target_offset=min(len(image.data),offset+chunk*WINDOW)
                    send_offset=offset
                    while send_offset<target_offset:
                        part=image.data[send_offset:send_offset+chunk]
                        await client.write_gatt_char(DATA,struct.pack("<I",send_offset)+part,response=False)
                        send_offset+=len(part)
                    while True:
                        (code,expected,detail),recovered=await wait_for_status(queue,client)
                        if code==2 and expected>=target_offset: offset=expected; break
                        if code in (2,0x82,0x83): offset=expected; break
                        if code>=0x80: raise RuntimeError(f"DATA failed: {NAMES.get(code)}, detail={detail}")
                    print(f"\r  {offset/len(image.data):6.1%} {offset/max(time.monotonic()-started,.001)/1024:8.1f} KiB/s",end="")
                print(); status=await command(b"\x01\x02",{3})
                if status[0]!=3: raise RuntimeError(f"FINISH failed: {NAMES.get(status[0])}")
            status=await command(b"\x01\x11",{5})
            if status[0]!=5: raise RuntimeError(f"playlist commit failed: {NAMES.get(status[0])}")
            status=await command(b"\x01\x04",{4})
            if status[0]!=4: raise RuntimeError(f"APPLY failed: {NAMES.get(status[0])}")
            print("Playlist committed; APPLY sent once")
    except Exception as error:
        raise RuntimeError(f"sync interrupted: {error}; reconnect and rerun to resume completed slots") from error

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--file",type=Path,action="append",required=True,help="ordered Y8 file; repeat 1-5 times")
    parser.add_argument("--revision",type=int); parser.add_argument("--interval",type=int,default=300)
    parser.add_argument("--name"); args=parser.parse_args()
    if not 1<=len(args.file)<=MAX_IMAGES: parser.error("--file must be repeated 1-5 times")
    asyncio.run(synchronize(args))
if __name__=="__main__":main()

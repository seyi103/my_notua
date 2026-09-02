#!/usr/bin/env python3
"""Incrementally synchronize 1-5 pre-generated Y8 images and one ordered playlist."""
from __future__ import annotations
import argparse, asyncio, importlib.metadata, struct, sys, time, zlib
from dataclasses import dataclass
from pathlib import Path
from bleak import BleakClient, BleakScanner

SERVICE="7d2a4b70-8e67-4d8b-9f3a-36c89e210001"
CONTROL="7d2a4b70-8e67-4d8b-9f3a-36c89e210003"
DATA="7d2a4b70-8e67-4d8b-9f3a-36c89e210004"
STATUS="7d2a4b70-8e67-4d8b-9f3a-36c89e210005"
CATALOG="7d2a4b70-8e67-4d8b-9f3a-36c89e210006"
IMAGE_BYTES=1_920_000; MAX_IMAGES=5; WINDOW=8; PLAYLIST_BYTES=35
MIN_INTERVAL_SECONDS=60; MAX_INTERVAL_SECONDS=24*60*60; DEFAULT_INTERVAL_SECONDS=300
MAX_WRITE_POLL_SECONDS=10.0; MAX_WRITE_POLL_INTERVAL_SECONDS=0.5
DATA_WRITE_TIMEOUT_SECONDS=10.0
NAMES={1:"START_ACCEPTED",2:"ACK",3:"COMMITTED",4:"APPLYING",5:"PLAYLIST_COMMITTED",6:"SYNC_ACCEPTED",
0x80:"BAD_COMMAND",0x81:"BAD_SIZE",0x82:"BAD_OFFSET",0x83:"QUEUE_FULL",0x84:"CRC_MISMATCH",
0x85:"STORAGE_ERROR",0x86:"NOT_READY"}

@dataclass
class Image: path: Path; data: bytes; crc: int; slot: int=-1

@dataclass
class TransferDiagnostics:
    notifications: int=0
    notification_timeouts: int=0
    status_reads: int=0
    ack_count: int=0
    ack_latency_total: float=0.0
    ack_latency_max: float=0.0

    def record_ack(self, latency:float)->None:
        self.ack_count+=1; self.ack_latency_total+=latency
        self.ack_latency_max=max(self.ack_latency_max,latency)

    def print_summary(self)->None:
        average=self.ack_latency_total/self.ack_count if self.ack_count else 0.0
        print(f"TransferDiagnostics: notifications={self.notifications}, "
              f"notification_timeouts={self.notification_timeouts}, "
              f"fallback_status_reads={self.status_reads}, "
              f"ACK_latency_avg={average*1000:.1f} ms, ACK_latency_max={self.ack_latency_max*1000:.1f} ms")

def decode_status(raw: bytes):
    if len(raw)!=12: raise RuntimeError(f"status value is {len(raw)} bytes, expected 12")
    version,code,_reserved,expected,detail=struct.unpack("<BBHII",raw)
    if version!=1: raise RuntimeError(f"status version was {version}, expected 1")
    return code,expected,detail

def encode_playlist(revision:int, images:list[Image], interval:int)->bytes:
    if not 1<=len(images)<=MAX_IMAGES: raise ValueError("playlist requires 1-5 images")
    if not MIN_INTERVAL_SECONDS<=interval<=MAX_INTERVAL_SECONDS:
        raise ValueError(f"interval requires {MIN_INTERVAL_SECONDS}..{MAX_INTERVAL_SECONDS} seconds")
    slots=[image.slot for image in images]+[0]*(MAX_IMAGES-len(images))
    crcs=[image.crc for image in images]+[0]*(MAX_IMAGES-len(images))
    return struct.pack("<BBII5B5I",1,len(images),revision,interval,*slots,*crcs)

def decode_playlist(raw:bytes):
    version,count,revision,interval,*values=struct.unpack("<BBII5B5I",raw)
    return {"version":version,"count":count,"revision":revision,"interval":interval,
            "slots":values[:5],"crcs":values[5:]}

def playlist_matches(requested:bytes, stored:dict)->bool:
    """Compare every encoded target field, including unused fixed-width entries."""
    return requested == struct.pack("<BBII5B5I", stored["version"], stored["count"],
        stored["revision"], stored["interval"], *stored["slots"], *stored["crcs"])

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

async def wait_for_status(queue,client,timeout=15.0,diagnostics=None):
    try:return await asyncio.wait_for(queue.get(),timeout),False
    except asyncio.TimeoutError:
        if diagnostics:
            diagnostics.notification_timeouts+=1; diagnostics.status_reads+=1
        print("\nNotification-timeout fallback: reading persisted Transfer Status")
        status=decode_status(bytes(await client.read_gatt_char(STATUS)))
        print(f"Fallback STATUS read: {NAMES.get(status[0],hex(status[0]))} persisted_offset={status[1]:,}")
        return status,True

async def wait_for_max_write(characteristic,allow_slow=False,timeout=MAX_WRITE_POLL_SECONDS,
                             poll_interval=MAX_WRITE_POLL_INTERVAL_SECONDS):
    """Wait for Bleak's backend to publish the negotiated write-without-response size."""
    deadline=time.monotonic()+timeout
    maximum=characteristic.max_write_without_response_size
    while maximum==20 and time.monotonic()<deadline:
        await asyncio.sleep(poll_interval)
        maximum=characteristic.max_write_without_response_size
    if maximum==20 and not allow_slow:
        raise RuntimeError("max_write_without_response_size remained at Bleak's default 20 bytes "
                           f"for {timeout:g} seconds; MTU negotiation did not become visible. "
                           "Refusing a multi-hour transfer. Use --allow-slow-write only for diagnostics")
    if maximum<=4: raise RuntimeError(f"invalid max_write_without_response_size: {maximum}")
    return maximum

async def write_data_packet(client,payload:bytes,packet_offset:int,window_start:int,target_offset:int,
                            diagnostics:TransferDiagnostics,timeout=DATA_WRITE_TIMEOUT_SECONDS):
    started=time.monotonic()
    try:
        await asyncio.wait_for(client.write_gatt_char(DATA,payload,response=False),timeout)
    except asyncio.TimeoutError as error:
        elapsed=time.monotonic()-started
        raise RuntimeError(
            "DATA write timeout: "
            f"packet_offset={packet_offset}, packet_length={len(payload)}, "
            f"window_start={window_start}, window_target={target_offset}, "
            f"elapsed={elapsed:.3f}s, notifications={diagnostics.notifications}, "
            f"notification_timeouts={diagnostics.notification_timeouts}, "
            f"fallback_status_reads={diagnostics.status_reads}") from error

async def find_device(name):
    for device,advertisement in (await BleakScanner.discover(timeout=10,return_adv=True)).values():
        if device.name==(name or "Notua") or SERVICE in {u.lower() for u in advertisement.service_uuids}: return device
    raise RuntimeError("Notua not found")

async def synchronize(args):
    diagnostics=TransferDiagnostics()
    try:
        images=[]
        for path in args.file:
            data=path.read_bytes()
            if len(data)!=IMAGE_BYTES: raise ValueError(f"{path}: expected {IMAGE_BYTES:,} bytes")
            images.append(Image(path,data,zlib.crc32(data)&0xffffffff))
        if len({image.crc for image in images})!=len(images): raise ValueError("duplicate image CRCs are not supported")
        queue=asyncio.Queue(); device=await find_device(args.name)
        def notified(_sender,value):
            try:
                diagnostics.notifications+=1
                queue.put_nowait(decode_status(bytes(value)))
            except RuntimeError as error: print(f"Ignored status: {error}")
        async with BleakClient(device,timeout=20) as client:
            await client.start_notify(STATUS,notified)
            data_characteristic=client.services.get_characteristic(DATA)
            if data_characteristic is None: raise RuntimeError("Data characteristic was not discovered")
            maximum=await wait_for_max_write(data_characteristic,args.allow_slow_write)
            chunk=min(maximum,512)-4
            try: bleak_version=importlib.metadata.version("bleak")
            except importlib.metadata.PackageNotFoundError: bleak_version="unknown"
            backend=type(getattr(client,"_backend",client)).__name__
            print(f"Sender BLE: bleak={bleak_version}, platform={sys.platform}, backend={backend}")
            print(f"Negotiated sender parameters: max_write_without_response_size={maximum}, "
                  f"payload_chunk_size={chunk}, window_packets={WINDOW}, "
                  f"bytes_per_window={chunk*WINDOW}")
            async def command(packet,wanted):
                while not queue.empty(): queue.get_nowait()
                await client.write_gatt_char(CONTROL,packet,response=True)
                while True:
                    status,_=await wait_for_status(queue,client,diagnostics=diagnostics)
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
            continuing=catalog["stage"]==1 and playlist_matches(target,catalog["target"])
            if catalog["stage"]==1 and not continuing:
                raise RuntimeError("device has an incomplete synchronization with a different complete target; "
                                   "rerun with the original files/order/revision/interval and finish it first")
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
                    window_start=offset
                    target_offset=min(len(image.data),offset+chunk*WINDOW)
                    send_offset=offset
                    while send_offset<target_offset:
                        part=image.data[send_offset:send_offset+chunk]
                        packet=struct.pack("<I",send_offset)+part
                        await write_data_packet(client,packet,send_offset,window_start,target_offset,diagnostics)
                        send_offset+=len(part)
                    ack_started=time.monotonic()
                    while True:
                        (code,expected,detail),recovered=await wait_for_status(
                            queue,client,diagnostics=diagnostics)
                        if code==2: diagnostics.record_ack(time.monotonic()-ack_started)
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
    finally:
        diagnostics.print_summary()

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--file",type=Path,action="append",required=True,help="ordered Y8 file; repeat 1-5 times")
    parser.add_argument("--revision",type=int)
    parser.add_argument("--interval",type=int,default=DEFAULT_INTERVAL_SECONDS)
    parser.add_argument("--allow-slow-write",action="store_true",
                        help="permit the 20-byte Bleak default only for throughput diagnostics")
    parser.add_argument("--name"); args=parser.parse_args()
    if not 1<=len(args.file)<=MAX_IMAGES: parser.error("--file must be repeated 1-5 times")
    if not MIN_INTERVAL_SECONDS<=args.interval<=MAX_INTERVAL_SECONDS:
        parser.error(f"--interval must be {MIN_INTERVAL_SECONDS}..{MAX_INTERVAL_SECONDS} seconds")
    asyncio.run(synchronize(args))
if __name__=="__main__":main()
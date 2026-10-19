"""仅对 busy 拒绝计账，不根据采样深度推断因果。"""


def classify(rows, start_us, end_us):
    counts=[0,0,0]
    events=[]
    previous=[0,0,0]
    dropped=0
    for row in sorted(rows,key=lambda r:r['monotonicUs']):
        busy=row['busy']
        current=busy['counts']
        if len(current)!=3 or any(b<a for a,b in zip(previous,current)):
            raise ValueError('busy counter reset or invalid cardinality')
        previous=current
        dropped=max(dropped,busy['dropped'])
        for event in busy['events']:
            if start_us<=event['monotonicUs']<end_us:
                counts[event['reason']]+=1
                events.append(event)
    events.sort(key=lambda e:e['monotonicUs'])
    return dict(windowCounts=counts,finalCounts=previous,dropped=dropped,complete=dropped==0,
                firstOffsetUs=events[0]['monotonicUs']-start_us if events else None,events=events)


def stop_reason(sent, errors, high_error_seconds, no_success_seconds):
    if sent and errors/sent>.1 and high_error_seconds>=10:
        return 'error-rate'
    if no_success_seconds>=30:
        return 'no-success'
    return None


def correlate(events, mappings, clients):
    routes={}
    ambiguous=set()
    for m in mappings:
        key=(m['gatewayId'],m['clientSession'],m['seq'])
        if key in routes and routes[key]!=m['requestId']: ambiguous.add(key)
        routes[key]=m['requestId']
    server={}
    for event in events:
        route=(event['gatewayId'],event['clientSession'],event['seq'])
        if route in routes and route not in ambiguous:
            key=(event['playerId'],routes[route])
            server.setdefault(key,[]).append(event)
    pairs=[]
    seen=set()
    duplicate=0
    for client in clients:
        key=(client['playerId'],client['requestId'])
        if key in seen: duplicate+=1
        seen.add(key)
        matches=server.get(key,[])
        if len(matches)==1 and client['receivedUs']>=matches[0]['monotonicUs']:
            pairs.append(dict(client=client,server=matches[0]))
    return dict(serverEvents=len(events),clientEvents=len(clients),gatewayMapped=sum(map(len,server.values())),
                matched=len(pairs),duplicateClient=duplicate,pairs=pairs,
                complete=len(pairs)==len(events)==len(clients) and not duplicate)
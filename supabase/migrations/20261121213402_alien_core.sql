-- Alien displays: core schema (as applied to project vvnrjlsqtfieftvjenhf)
-- Devices never log in as users: each holds a random token (only its hash is stored)
-- and uses device_register / device_poll. People claim displays with claim_device.
create extension if not exists pgcrypto;

create table public.profiles (
  id           uuid primary key references auth.users on delete cascade,
  display_name text not null,
  is_admin     boolean not null default false,
  created_at   timestamptz not null default now()
);

create table public.devices (
  id          uuid primary key default gen_random_uuid(),
  chip_id     text not null unique,
  name        text not null default 'New alien',
  token_hash  text not null,
  claim_code  text unique,
  claimed_by  uuid references public.profiles(id) on delete set null,
  fw_version  text,
  fw_channel  text not null default 'stable' check (fw_channel in ('test', 'stable')),
  timezone    text not null default 'America/Chicago',
  settings    jsonb not null default '{}'::jsonb,
  last_seen   timestamptz,
  created_at  timestamptz not null default now()
);

create table public.device_access (
  device_id uuid references public.devices on delete cascade,
  user_id   uuid references public.profiles on delete cascade,
  primary key (device_id, user_id)
);

create table public.messages (
  id           bigint generated always as identity primary key,
  device_id    uuid not null references public.devices on delete cascade,
  sender_id    uuid references public.profiles(id) on delete set null,
  kind         text not null default 'message' check (kind in ('message', 'mood', 'command')),
  text         text check (char_length(text) <= 140),
  mood         text,
  repeats      smallint not null default 3 check (repeats between 1 and 5),
  deliver_at   timestamptz not null default now(),
  delivered_at timestamptz,
  created_at   timestamptz not null default now()
);
create index messages_pending on public.messages (device_id, deliver_at) where delivered_at is null;
create index messages_sender on public.messages (sender_id);
create index devices_claimed_by on public.devices (claimed_by);
create index device_access_user on public.device_access (user_id);

create table public.firmware_releases (
  id           bigint generated always as identity primary key,
  version      text not null unique,
  channel      text not null default 'test' check (channel in ('test', 'stable')),
  notes        text,
  storage_path text not null,
  sha256       text not null,
  size_bytes   integer,
  published    boolean not null default false,
  created_at   timestamptz not null default now()
);

create or replace function public.is_admin() returns boolean
language sql stable security definer set search_path = public as $$
  select coalesce((select is_admin from public.profiles where id = auth.uid()), false);
$$;

create or replace function public.can_use_device(d uuid) returns boolean
language sql stable security definer set search_path = public as $$
  select public.is_admin()
      or exists (select 1 from public.devices where id = d and claimed_by = auth.uid())
      or exists (select 1 from public.device_access where device_id = d and user_id = auth.uid());
$$;

create or replace function public.handle_new_user() returns trigger
language plpgsql security definer set search_path = public as $$
begin
  insert into public.profiles (id, display_name)
  values (new.id, coalesce(new.raw_user_meta_data->>'display_name', split_part(new.email, '@', 1)));
  return new;
end $$;
create trigger on_auth_user_created after insert on auth.users
  for each row execute function public.handle_new_user();

alter table public.profiles          enable row level security;
alter table public.devices           enable row level security;
alter table public.device_access     enable row level security;
alter table public.messages          enable row level security;
alter table public.firmware_releases enable row level security;

create policy "family can see profiles" on public.profiles
  for select to authenticated using (true);
create policy "edit own profile" on public.profiles
  for update to authenticated using (id = (select auth.uid()))
  with check (id = (select auth.uid()) and is_admin = public.is_admin());

create policy "see my displays" on public.devices
  for select to authenticated using (public.can_use_device(id));
create policy "owner edits display" on public.devices
  for update to authenticated using (claimed_by = (select auth.uid()) or public.is_admin());
create policy "owner removes display" on public.devices
  for delete to authenticated using (claimed_by = (select auth.uid()) or public.is_admin());

create policy "see access lists" on public.device_access
  for select to authenticated using (public.can_use_device(device_id));
create policy "owner grants access" on public.device_access
  for insert to authenticated with check (
    exists (select 1 from public.devices where id = device_id and claimed_by = (select auth.uid())) or public.is_admin());
create policy "owner revokes access" on public.device_access
  for delete to authenticated using (
    exists (select 1 from public.devices where id = device_id and claimed_by = (select auth.uid())) or public.is_admin());

create policy "see messages for my displays" on public.messages
  for select to authenticated using (public.can_use_device(device_id));
create policy "send to my displays" on public.messages
  for insert to authenticated with check (sender_id = (select auth.uid()) and public.can_use_device(device_id));
create policy "cancel my unsent messages" on public.messages
  for delete to authenticated using (sender_id = (select auth.uid()) and delivered_at is null);

create policy "see releases" on public.firmware_releases
  for select to authenticated using (true);
create policy "admin manages releases" on public.firmware_releases
  for all to authenticated using (public.is_admin()) with check (public.is_admin());

create or replace function public.device_register(p_chip_id text)
returns json language plpgsql security definer set search_path = public, extensions as $$
declare
  v_token text := encode(gen_random_bytes(24), 'hex');
  v_code  text;
  v_dev   public.devices;
begin
  if p_chip_id is null or length(p_chip_id) < 6 then raise exception 'bad chip id'; end if;
  select * into v_dev from public.devices where chip_id = p_chip_id;
  if found and v_dev.claimed_by is not null then
    raise exception 'device already claimed';
  end if;
  loop
    v_code := (select string_agg(substr('ABCDEFGHJKLMNPQRSTUVWXYZ23456789', 1 + floor(random() * 32)::int, 1), '')
               from generate_series(1, 6));
    exit when not exists (select 1 from public.devices where claim_code = v_code);
  end loop;
  insert into public.devices (chip_id, token_hash, claim_code)
  values (p_chip_id, encode(digest(v_token, 'sha256'), 'hex'), v_code)
  on conflict (chip_id) do update
    set token_hash = excluded.token_hash, claim_code = excluded.claim_code;
  return json_build_object('token', v_token, 'claim_code', v_code);
end $$;

create or replace function public.device_poll(p_token text, p_fw_version text default null)
returns json language plpgsql security definer set search_path = public, extensions as $$
declare
  v_dev  public.devices;
  v_msgs json;
  v_fw   public.firmware_releases;
begin
  update public.devices
     set last_seen = now(), fw_version = coalesce(p_fw_version, fw_version)
   where token_hash = encode(digest(p_token, 'sha256'), 'hex')
  returning * into v_dev;
  if not found then raise exception 'unknown device'; end if;

  if v_dev.claimed_by is null then
    return json_build_object('claimed', false, 'claim_code', v_dev.claim_code);
  end if;

  with due as (
    select id from public.messages
     where device_id = v_dev.id and delivered_at is null and deliver_at <= now()
     order by deliver_at, id limit 5
     for update skip locked
  ), sent as (
    update public.messages m set delivered_at = now()
      from due where m.id = due.id
    returning m.id, m.kind, m.text, m.mood, m.repeats, m.deliver_at
  )
  select coalesce(json_agg(sent order by sent.deliver_at, sent.id), '[]'::json) into v_msgs from sent;

  select * into v_fw from public.firmware_releases
   where published and (channel = v_dev.fw_channel or channel = 'stable')
   order by created_at desc limit 1;

  return json_build_object(
    'claimed',  true,
    'name',     v_dev.name,
    'timezone', v_dev.timezone,
    'settings', v_dev.settings,
    'messages', v_msgs,
    'firmware', case when v_fw.id is not null and v_fw.version is distinct from p_fw_version
                     then json_build_object('version', v_fw.version, 'path', v_fw.storage_path,
                                            'sha256', v_fw.sha256, 'size', v_fw.size_bytes)
                end
  );
end $$;

create or replace function public.claim_device(p_code text, p_name text default null)
returns uuid language plpgsql security definer set search_path = public as $$
declare v_id uuid;
begin
  if auth.uid() is null then raise exception 'sign in first'; end if;
  update public.devices
     set claimed_by = auth.uid(), claim_code = null, name = coalesce(nullif(trim(p_name), ''), name)
   where claim_code = upper(trim(p_code)) and claimed_by is null
  returning id into v_id;
  if v_id is null then raise exception 'no display is showing that code'; end if;
  insert into public.device_access (device_id, user_id) values (v_id, auth.uid()) on conflict do nothing;
  return v_id;
end $$;

revoke all on function public.device_register(text)       from public, anon, authenticated;
revoke all on function public.device_poll(text, text)     from public, anon, authenticated;
revoke all on function public.claim_device(text, text)    from public, anon, authenticated;
revoke all on function public.handle_new_user()           from public, anon, authenticated;
grant execute on function public.device_register(text)    to anon;
grant execute on function public.device_poll(text, text)  to anon;
grant execute on function public.claim_device(text, text) to authenticated;

-- firmware files: public read (displays download them), only admins upload
insert into storage.buckets (id, name, public) values ('firmware', 'firmware', true)
  on conflict (id) do nothing;
create policy "admins upload firmware" on storage.objects
  for insert to authenticated with check (bucket_id = 'firmware' and public.is_admin());
create policy "admins replace firmware" on storage.objects
  for update to authenticated using (bucket_id = 'firmware' and public.is_admin());
create policy "admins delete firmware" on storage.objects
  for delete to authenticated using (bucket_id = 'firmware' and public.is_admin());
